"""Hardware regression: different PWM frequencies must not share an LEDC timer."""
from itertools import combinations
from pathlib import Path
import unittest

import esphome.config_validation as cv
import yaml


class DisplayHardwareTest(unittest.TestCase):
    def test_independent_pwm_frequencies_have_independent_timers(self):
        hardware = Path(__file__).resolve().parents[1] / 'packages/hardware.yaml'
        config = yaml.safe_load(hardware.read_text())
        outputs = [item for item in config['output'] if item['platform'] == 'ledc']
        for first, second in combinations(outputs, 2):
            if cv.frequency(first.get('frequency', '1kHz')) == cv.frequency(second.get('frequency', '1kHz')):
                continue
            with self.subTest(first=first['id'], second=second['id']):
                self.assertIn('channel', first)
                self.assertIn('channel', second)
                self.assertNotEqual(first['channel'] // 2, second['channel'] // 2,
                                    'Adjacent channels share a timer and cannot use different PWM frequencies')


if __name__ == '__main__':
    unittest.main()
