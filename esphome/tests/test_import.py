"""Validate adoption against an immutable local Git tag, including nested includes."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ImportTest(unittest.TestCase):
    def test_tagged_package_from_an_unrelated_user_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            repo = temporary / 'source'
            repo.mkdir()
            shutil.copytree(ROOT / 'esphome', repo / 'esphome',
                            ignore=shutil.ignore_patterns('.esphome', '__pycache__', 'build', 'secrets.yaml'))
            def run(*args):
                result = subprocess.run(args, cwd=repo, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result
            run('git', 'init', '-q')
            run(sys.executable, 'esphome/scripts/prepare_release.py', 'esphome-v0.1.0')
            run(sys.executable, 'esphome/scripts/prepare_release.py', 'esphome-v0.1.0', '--check')
            mismatch = subprocess.run([sys.executable, 'esphome/scripts/prepare_release.py',
                                       'esphome-v0.2.0', '--check'], cwd=repo, capture_output=True)
            self.assertNotEqual(mismatch.returncode, 0)
            run('git', 'add', 'esphome')
            run('git', '-c', 'commit.gpgsign=false', '-c', 'user.name=ESPHome test', '-c', 'user.email=test@example.invalid',
                'commit', '-qm', 'Immutable package fixture')
            run('git', 'tag', 'esphome-v0.1.0')
            user = temporary / 'user'
            user.mkdir()
            config = user / 'hallway.yaml'
            config.write_text(f'''substitutions:
  name: hallway
  friendly_name: Hallway
packages:
  thermostat:
    url: {repo.as_uri()}
    ref: esphome-v0.1.0
    files: [esphome/smart-thermostat.yaml]
    refresh: never
esphome:
  name_add_mac_suffix: false
api:
  encryption:
    key: AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=
wifi:
  ssid: example-test-network
  password: example-test-password
''')
            result = subprocess.run([sys.executable, '-m', 'esphome', 'config', str(config)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('name: hallway', result.stdout)
            self.assertIn('version: 0.1.0', result.stdout)
            self.assertIn('id: start_compressor', result.stdout)
            self.assertIn('esphome-v0.1.0', result.stdout)


if __name__ == '__main__':
    unittest.main()
