"""Validate touch targets and UI entity bindings."""
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest


def dictionaries(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from dictionaries(child)
    elif isinstance(value, list):
        for child in value:
            yield from dictionaries(child)


def tracks(values, available, gap):
    available -= gap * (len(values) - 1)
    fixed = sum(v for v in values if isinstance(v, int))
    weights = [int(re.fullmatch(r'LV_GRID_FR\((\d+)\)', v)[1]) if isinstance(v, str) else 0
               for v in values]
    assert fixed <= available, 'Grid exceeds the available screen size'
    return [v if isinstance(v, int) else (available - fixed) * weight / sum(weights)
            for v, weight in zip(values, weights)]


class TapUITest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        # Resolve !include vars using ESPHome itself, including shared page templates.
        script = '''import json, sys
from pathlib import Path
from esphome.core import CORE
from esphome.config import read_config
CORE.config_path = Path(sys.argv[1])
config = read_config({})
assert config is not None
print(json.dumps(config, default=str))
'''
        result = subprocess.run([sys.executable, '-c', script, str(root / 'smart-thermostat.yaml')],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.config = json.loads(result.stdout)
        cls.lvgl = cls.config['lvgl'][0]

    def test_navigation_does_not_require_dragging(self):
        for page in self.lvgl['pages']:
            self.assertIs(page['scrollable'], False, page['id'])
            self.assertEqual(page['layout']['type'], 'grid')
        for node in dictionaries(self.lvgl['pages']):
            for widget in ('dropdown', 'roller', 'slider'):
                self.assertNotIn(widget, node)
            if 'buttonmatrix' in node:
                self.assertIs(node['buttonmatrix']['scrollable'], False)

    def test_all_operating_modes_have_direct_buttons(self):
        modes = {node['climate.control']['mode'] for node in dictionaries(self.lvgl['pages'])
                 if 'climate.control' in node and 'mode' in node['climate.control']}
        self.assertEqual(modes, {'OFF', 'HEAT', 'COOL', 'HEAT_COOL', 'FAN_ONLY'})

    def test_every_button_uses_one_shared_beep(self):
        buttons = []
        for node in dictionaries(self.lvgl['pages']):
            if 'button' in node:
                buttons.append(node['button'])
            if 'buttonmatrix' in node:
                for row in node['buttonmatrix']['rows']:
                    buttons.extend(row['buttons'])
        self.assertGreater(len(buttons), 50)
        for button in buttons:
            actions = list(dictionaries(button['on_click']))
            feedback = [a for a in actions if a.get('script.execute', {}).get('id') == 'ui_beep']
            self.assertEqual(len(feedback), 1, button.get('id', button.get('text')))

    def test_grid_and_matrix_targets_fit_and_are_large_enough(self):
        for page in self.lvgl['pages']:
            layout = page['layout']
            # Every page uses the shared 8 px screen padding; theme removes matrix borders/padding.
            self.assertEqual(page['styles'], ['screen'])
            rows = tracks(layout['grid_rows'], 240 - 16, layout['pad_row'])
            columns = tracks(layout['grid_columns'], 320 - 16, layout['pad_column'])
            occupied = set()
            for widget in page['widgets']:
                kind, content = next(iter(widget.items()))
                row, col = content['grid_cell_row_pos'], content['grid_cell_column_pos']
                nr, nc = content.get('grid_cell_row_span', 1), content.get('grid_cell_column_span', 1)
                self.assertLessEqual(row + nr, len(rows))
                self.assertLessEqual(col + nc, len(columns))
                for r in range(row, row + nr):
                    for c in range(col, col + nc):
                        self.assertNotIn((r, c), occupied, page['id'])
                        occupied.add((r, c))
                width = sum(columns[col:col + nc]) + layout['pad_column'] * (nc - 1)
                height = sum(rows[row:row + nr]) + layout['pad_row'] * (nr - 1)
                if kind == 'button':
                    self.assertGreaterEqual(height, 40)
                if kind != 'buttonmatrix':
                    continue
                matrix_rows = content['rows']
                matrix_style = {**self.lvgl['theme']['buttonmatrix'], **content}
                key_height = (height - matrix_style['pad_row'] * (len(matrix_rows) - 1)) / len(matrix_rows)
                self.assertGreaterEqual(key_height, 40, (page['id'], key_height))
                for matrix_row in matrix_rows:
                    buttons = matrix_row['buttons']
                    units = sum(b['width'] for b in buttons)
                    for button in buttons:
                        key_width = (width - matrix_style['pad_column'] * (len(buttons) - 1)) * button['width'] / units
                        self.assertGreaterEqual(key_width, 55, (page['id'], button['text']))

    def test_wifi_qr_size_and_scanning_margin(self):
        page = next(p for p in self.lvgl['pages'] if p['id'] == 'wifi_setup_page')
        qr = next(w['qrcode'] for w in page['widgets'] if 'qrcode' in w)
        self.assertEqual(qr['size'], 150)
        self.assertEqual(qr['width'], 150)
        self.assertEqual(qr['height'], 150)
        self.assertEqual(qr['border_width'], 12)

    def test_settings_bind_directly_to_existing_entities(self):
        pages = {p['id'] for p in self.lvgl['pages']}
        selects = {s['id']: set(s['options']) for s in self.config['select']}
        actual = {}
        for node in dictionaries(self.lvgl['pages']):
            if 'select.set' in node:
                action = node['select.set']
                self.assertIn(action['option'], selects[action['id']])
                actual.setdefault(action['id'], set()).add(action['option'])
        for entity in ('equipment', 'compressor_stages', 'valve_polarity', 'heating_fan',
                       'backup_type', 'backup_policy', 'display_units', 'outdoor_units'):
            self.assertEqual(actual[entity], selects[entity])
            self.assertIn(entity + '_page', pages)
        for entity in ('temperature_offset', 'humidity_offset', 'stage_delta', 'stage_delay',
                       'valve_pre_change_delay', 'valve_post_change_delay', 'retain_valve_on_stop',
                       'balance_point', 'sleep_seconds',
                       'auto_backup', 'balance_confirmed', 'beep_enabled', 'auto_brightness'):
            self.assertIn(entity + '_page', pages)


if __name__ == '__main__':
    unittest.main()
