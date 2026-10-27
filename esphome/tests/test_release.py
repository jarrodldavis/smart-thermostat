"""Exercise official manifest preservation, artifact selection and release pinning."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_release', ROOT / 'scripts/package_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTest(unittest.TestCase):
    def fixture(self, build):
        build.mkdir()
        entries = []
        for name, data in [('device.factory.bin', b'merged'), ('device.ota.bin', b'ota')]:
            (build / name).write_bytes(data)
            entries.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest()})
        manifest = {'name': 'smeisner.smart-thermostat', 'version': '0.1.0',
                    'builds': [{'chipFamily': 'ESP32-S3',
                                'parts': [dict(entries[0], offset=0)], 'ota': entries[1]}]}
        (build / 'manifest.json').write_text(json.dumps(manifest))
        (build / 'private-photo.jpg').write_bytes(b'private')
        return manifest

    def test_official_manifest_and_checksums_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, out = root / 'build', root / 'site'
            manifest = self.fixture(build)
            self.assertEqual(release.package(build, out, '2026.10.0'), manifest)
            self.assertEqual((out / 'manifest.json').read_bytes(),
                             (build / 'manifest.json').read_bytes())
            self.assertEqual((out / 'device.ota.bin').read_bytes(), b'ota')
            self.assertFalse((out / 'private-photo.jpg').exists())
            for entry in (out / 'SHA256SUMS').read_text().splitlines():
                digest, name = entry.split('  ')
                self.assertEqual(hashlib.sha256((out / name).read_bytes()).hexdigest(), digest)
            metadata = json.loads((out / 'version.json').read_text())
            self.assertEqual(metadata['project'], 'smeisner.smart-thermostat')
            self.assertEqual(metadata['version'], '0.1.0')
            self.assertEqual(metadata['esphome'], '2026.10.0')

    def test_cannot_package_files_outside_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            manifest = self.fixture(build)
            (root / 'private.bin').write_bytes(b'merged')
            manifest['builds'][0]['parts'][0]['path'] = '../private.bin'
            (build / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                release.package(build, root / 'out', 'test')

    def test_corrupt_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            self.fixture(build)
            (build / 'device.ota.bin').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                release.package(build, root / 'out', 'test')

    def test_no_mismatched_release_ref(self):
        config = (ROOT / 'smart-thermostat.yaml').read_text()
        self.assertIn('smeisner.smart-thermostat', config)
        self.assertIn('@${package_ref}', config)
        self.assertIn('import_full_config: false', config)
        self.assertNotIn('!secret', config)


if __name__ == '__main__':
    unittest.main()
