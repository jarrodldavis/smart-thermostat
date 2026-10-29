"""Staging and touchscreen integration contracts."""
from pathlib import Path
import subprocess
import sys
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[1]


class ConfigLoader(yaml.SafeLoader):
    pass


ConfigLoader.add_constructor('!lambda', lambda loader, node: loader.construct_scalar(node))
ConfigLoader.add_constructor('!include', lambda loader, node: loader.construct_scalar(node))


class ComponentsTest(unittest.TestCase):
    def test_thermostat_actions_drive_stage_requests(self):
        config = yaml.load((ROOT / 'packages/climate.yaml').read_text(), Loader=ConfigLoader)
        thermostat = config['climate'][0]
        self.assertIs(thermostat['startup_delay'], True)
        self.assertEqual(thermostat['min_idle_time'], '30s')
        self.assertEqual(thermostat['supplemental_heating_delta'], 1)
        self.assertEqual(thermostat['supplemental_cooling_delta'], 1)
        self.assertEqual(thermostat['max_heating_run_time'], '5min')
        self.assertEqual(thermostat['max_cooling_run_time'], '5min')
        for action in ('heat_action', 'cool_action', 'idle_action', 'fan_only_action'):
            self.assertEqual(thermostat[action], [
                {'script.execute': 'reset_stage_requests'},
                {'script.execute': 'apply_hvac'},
            ])
        for action in ('supplemental_heating_action', 'supplemental_cooling_action'):
            self.assertEqual(thermostat[action][-1], {'script.execute': 'apply_hvac'})
            self.assertNotIn('lambda', yaml.dump(thermostat[action]))
            self.assertIn('stage2_requested', yaml.dump(thermostat[action]))
        self.assertIn('local_backup_requested', yaml.dump(thermostat['supplemental_heating_action']))
        for mode, operation in [('on', 'turn_on'), ('auto', 'turn_off')]:
            self.assertEqual(thermostat[f'fan_mode_{mode}_action'], [
                {f'switch.{operation}': 'continuous_fan_requested'},
                {'script.execute': 'apply_hvac'},
            ])
        for entity in ('stage_delta', 'stage_delay'):
            number = next(n for n in config['number'] if n['id'] == entity)
            self.assertEqual(number['on_value'], [
                {'script.execute': 'equipment_changed'},
                {'script.execute': 'configure_staging'},
            ], 'Inhibit relays before changing a running thermostat timer')
        self.assertEqual({g['id'] for g in config['globals']},
                         {'commissioned', 'runtime_ready', 'updating'})
        self.assertNotIn('includes', config['esphome'])
        self.assertNotIn('interval', config)

    def test_scripts_enforce_interlocks_and_cancel_pending_starts(self):
        outputs = yaml.load((ROOT / 'packages/climate/outputs.yaml').read_text(), Loader=ConfigLoader)
        scripts = {s['id']: s for s in outputs['script']}
        for script, delay in [('compressor_cooldown', '${compressor_off_time}'),
                              ('stage2_cooldown', '${stage2_off_time}'),
                              ('backup_settle', '${source_settle_time}')]:
            self.assertEqual(scripts[script]['mode'], 'restart')
            self.assertEqual(scripts[script]['then'][0], {'delay': delay})
        stops = scripts['stop_heat_sources']['then']
        self.assertEqual({a['script.stop'] for a in stops if 'script.stop' in a},
                         {'start_compressor', 'start_stage2', 'start_backup', 'change_valve'})
        self.assertLess(stops.index({'switch.turn_off': 'relay_y2'}),
                        stops.index({'switch.turn_off': 'relay_y1'}))
        for script in ('start_compressor', 'start_stage2', 'start_backup'):
            self.assertIn('wait_until', yaml.dump(scripts[script]))
            self.assertIn('commissioned', yaml.dump(scripts[script]))
        self.assertTrue(all(s['internal'] for s in outputs['switch']))
        self.assertTrue(all(s['restore_mode'] == 'ALWAYS_OFF' for s in outputs['switch']))
        hardware = yaml.safe_load((ROOT / 'packages/hardware.yaml').read_text())
        pins = {o['id']: o.get('pin') for o in hardware['output']}
        relays = {s['id']: s for s in outputs['switch'] if s['platform'] == 'output'}
        for terminal, gpio in [('w', 5), ('y1', 6), ('y2', 41), ('g', 4), ('ob', 7), ('aux', 39)]:
            self.assertEqual(pins[relays['relay_' + terminal]['output']], f'GPIO{gpio}')
        self.assertFalse((ROOT / 'include/hvac_controller.h').exists())

    def test_ob_settings_use_independent_delays_and_retention(self):
        config = yaml.load((ROOT / 'packages/climate.yaml').read_text(), Loader=ConfigLoader)
        outputs = yaml.load((ROOT / 'packages/climate/outputs.yaml').read_text(), Loader=ConfigLoader)
        scripts = {s['id']: s for s in outputs['script']}
        for phase in ('pre', 'post'):
            number = next(n for n in config['number'] if n['id'] == f'valve_{phase}_change_delay')
            self.assertEqual((number['min_value'], number['max_value'], number['step']), (0, 300, 1))
            self.assertEqual(number['initial_value'], 30)
            self.assertEqual(number['unit_of_measurement'], 's')
            self.assertTrue(number['restore_value'])
            self.assertIn(f'valve_{phase}_change_delay', scripts[f'valve_{phase}_change']['then'][0]['delay'])
            self.assertEqual(scripts[f'valve_{phase}_change']['mode'], 'restart')
        retention = next(s for s in config['switch'] if s['id'] == 'retain_valve_on_stop')
        self.assertEqual(retention['restore_mode'], 'RESTORE_DEFAULT_ON')
        self.assertIn('retain_valve_on_stop', yaml.dump(scripts['stop_hvac']))
        self.assertNotIn({'switch.turn_off': 'relay_ob'}, scripts['stop_hvac']['then'])
        self.assertEqual(scripts['change_valve']['parameters'], {'energize': 'bool'})
        self.assertIn('valve_pre_change', yaml.dump(scripts['change_valve']))
        self.assertIn('change_valve', yaml.dump(scripts['start_compressor']))
        self.assertIn('valve_post_change', yaml.dump(scripts['start_compressor']))

    def test_default_irq_configuration_and_startup_transaction(self):
        result = subprocess.run([sys.executable, '-m', 'esphome', 'config',
                                 str(ROOT / 'smart-thermostat.yaml')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        config = yaml.load(result.stdout, Loader=ConfigLoader)
        touch = config['touchscreen'][0]
        self.assertEqual(touch['platform'], 'xpt2046')
        self.assertEqual(touch['interrupt_pin']['number'], 48)
        self.assertEqual(touch['touch_timeout'], '80ms')
        self.assertGreater(touch['threshold'], 496, 'Reject observed unattended noise spikes')
        self.assertFalse(any(s.get('id') == 'touch_irq_active' for s in config['binary_sensor']))
        hooks = config['esphome']['on_boot']
        prime = next(h for h in hooks if h['priority'] == 500)
        command = prime['then'][0]['lambda']
        self.assertIn('{0xD0, 0x00, 0x00}', command)
        self.assertLess(command.index('.enable()'), command.index('.transfer_array('))
        self.assertLess(command.index('.transfer_array('), command.index('.disable()'))
        # UI and equipment boot hooks need LVGL and restored controls ready.
        late = [h for h in hooks if h['priority'] == -100]
        actions = [a for hook in late for a in hook['then']]
        scripts = {a['script.execute']['id'] for a in actions if 'script.execute' in a}
        self.assertTrue({'wake_screen', 'refresh_ui', 'configure_staging'} <= scripts)
        self.assertTrue(any(a.get('globals.set', {}).get('id') == 'runtime_ready' for a in actions))



if __name__ == '__main__':
    unittest.main()
