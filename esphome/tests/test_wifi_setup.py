"""Native fallback setup retains password generation during safe-mode recovery."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class WiFiSetupTest(unittest.TestCase):
    def test_native_ap_and_password_helper_exist_before_safe_mode(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            config = temporary / 'setup.yaml'
            config.write_text(f"packages:\n  thermostat: !include {root / 'smart-thermostat.yaml'}\n")
            generate = """import sys
from pathlib import Path
from esphome.core import CORE
from esphome.config import read_config
from esphome.__main__ import generate_cpp_contents
CORE.config_path = Path(sys.argv[1])
config = read_config({})
assert config is not None
CORE.config = config
generate_cpp_contents(config)
print(CORE.cpp_main_section)
"""
            result = subprocess.run([sys.executable, '-c', generate, str(config)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            main = result.stdout
            recovery = main.index('->should_enter_safe_mode(')
            self.assertLess(main.index('new(setup_credentials)'), recovery,
                            'Safe mode must register the password helper before calling App.setup()')
            self.assertLess(main.index('->set_ap('), recovery,
                            'Safe mode must retain the native fallback AP')
            self.assertIn('wifi_setup_password::WiFiSetupPassword()', main)


if __name__ == '__main__':
    unittest.main()
