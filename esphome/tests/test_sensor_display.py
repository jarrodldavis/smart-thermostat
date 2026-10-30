"""Sensor freshness and presence-aware display lifecycle integration."""
import json
import math
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
READ_CONFIG = '''import json, sys
from pathlib import Path
from esphome.core import CORE
from esphome.config import read_config
CORE.config_path = Path(sys.argv[1])
config = read_config({})
assert config is not None
print(json.dumps(config, default=str))
'''


def read_config(path):
    result = subprocess.run([sys.executable, '-c', READ_CONFIG, str(path)], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return json.loads(result.stdout)



class SensorDisplayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = read_config(ROOT / 'smart-thermostat.yaml')

    def test_temperature_calibration_and_expiry_are_in_the_control_sensor_pipeline(self):
        aht = next(s for s in self.config['sensor'] if s['platform'] == 'aht10')
        temperature = aht['temperature']
        filters = temperature['filters']
        self.assertEqual([next(iter(f)) for f in filters],
                         ['lambda', 'exponential_moving_average', 'offset', 'timeout'])
        # The raw validity guard must precede smoothing, not discard invalid
        # samples after they have contaminated the accumulator.
        self.assertIn('std::isfinite(x)', filters[0]['lambda'])
        self.assertIn('-40.0f', filters[0]['lambda'])
        self.assertIn('85.0f', filters[0]['lambda'])
        self.assertIn('NAN', filters[0]['lambda'])
        self.assertEqual(filters[1]['exponential_moving_average']['alpha'], 0.1)
        self.assertIn('temperature_offset', filters[2]['offset'])
        self.assertEqual(filters[3]['timeout']['timeout'], '60s')
        self.assertTrue(math.isnan(filters[3]['timeout']['value']))
        thermostat = self.config['climate'][0]
        self.assertEqual(thermostat['sensor'], temperature['id'])

    def test_humidity_failure_remains_invalid_and_independent_of_temperature_control(self):
        aht = next(s for s in self.config['sensor'] if s['platform'] == 'aht10')
        filters = aht['humidity']['filters']
        self.assertEqual([next(iter(f)) for f in filters],
                         ['exponential_moving_average', 'offset', 'lambda', 'timeout'])
        self.assertIn('humidity_offset', filters[1]['offset'])
        self.assertIn('std::isfinite(x)', filters[2]['lambda'])
        self.assertIn('NAN', filters[2]['lambda'])
        self.assertTrue(math.isnan(filters[-1]['timeout']['value']))
        apply = next(s for s in self.config['script'] if s['id'] == 'apply_hvac')
        gate = apply['then'][0]['if']['condition']
        self.assertNotIn('room_humidity', json.dumps(gate))
        self.assertIn('std::isfinite(id(room_temperature).state)', json.dumps(gate))

    def test_outdoor_copy_subscribes_to_default_or_custom_sources(self):
        for config, source_id in [(self.config, 'outdoor_temperature'),
                                  (read_config(ROOT / 'tests/custom-outdoor.yaml'), 'local_outdoor_test')]:
            control = next(s for s in config['sensor'] if s.get('id') == 'outdoor_control_temperature')
            self.assertEqual(control['platform'], 'copy')
            self.assertEqual(control['source_id'], source_id)
            self.assertTrue(control['internal'])
            self.assertEqual(control['filters'][0]['timeout']['timeout'], '15min')
            self.assertTrue(math.isnan(control['filters'][0]['timeout']['value']))
            source = next(s for s in config['sensor'] if s.get('id') == source_id)
            self.assertNotIn('filters', source, 'Do not expire or mutate the original source sensor')

    def test_idle_rechecks_presence_and_drives_backlight_lifecycle(self):
        ui = self.config['lvgl'][0]
        interval = next(i for i in self.config['interval'] if str(i['interval']) == '1s')
        condition = interval['then'][0]['if']['condition']['and']
        idle = next(c['lvgl.is_idle'] for c in condition if 'lvgl.is_idle' in c)
        self.assertIn('sleep_seconds', idle['timeout'])
        self.assertEqual({c['binary_sensor.is_off']['id'] for c in condition if 'binary_sensor.is_off' in c},
                         {'presence_pin', 'radar_presence'})
        self.assertTrue(any('lvgl.is_paused' in c.get('not', {}) for c in condition))
        pause = ui['on_pause']['then']
        self.assertEqual(pause[0]['light.turn_off']['id'], 'screen_backlight')
        self.assertEqual(ui['on_resume']['then'][0]['script.execute']['id'], 'update_backlight')
        wake = next(s for s in self.config['script'] if s['id'] == 'wake_screen')
        self.assertEqual(len(wake['then']), 1)
        self.assertIn('lvgl.resume', wake['then'][0])
        # A waking touch is consumed until release.
        touch = self.config['touchscreen'][0]
        self.assertEqual(touch['on_release']['then'][0]['script.execute']['id'], 'wake_screen')
        for source in self.config['binary_sensor']:
            if source.get('id') == 'presence_pin':
                self.assertIn('wake_screen', json.dumps(source['on_press']))
            if source['platform'] == 'ld2410':
                self.assertIn('wake_screen', json.dumps(source['has_target']['on_press']))


if __name__ == '__main__':
    unittest.main()
