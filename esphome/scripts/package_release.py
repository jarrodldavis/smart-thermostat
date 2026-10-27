#!/usr/bin/env python3
"""Add metadata, checksums and installer assets to official ESPHome build output."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from urllib.parse import quote


def package(build: Path, destination: Path, esphome_version: str) -> dict:
    """Preserve official build-action artifacts; add project release information."""
    manifest = json.loads((build / 'manifest.json').read_text())
    builds = manifest['builds']
    if len(builds) != 1 or builds[0]['chipFamily'] != 'ESP32-S3':
        raise ValueError('Expected one ESP32-S3 build')
    entries = [*builds[0]['parts'], builds[0]['ota']]
    sources = []
    for entry in entries:
        name = entry['path']
        source = (build / name).resolve()
        if Path(name).name != name or not source.is_relative_to(build.resolve()):
            raise ValueError(f'Invalid artifact path: {name}')
        if hashlib.sha256(source.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError(f'Artifact checksum mismatch: {name}')
        sources.append(source)
    destination.mkdir(parents=True, exist_ok=True)
    for source in sources:
        shutil.copyfile(source, destination / source.name)
    shutil.copyfile(build / 'manifest.json', destination / 'manifest.json')
    metadata = {'project': 'smeisner.smart-thermostat', 'version': manifest['version'],
                'esphome': esphome_version, 'hardware': 'V0.7.3',
                'status': 'Requires board commissioning and hardware acceptance tests'}
    (destination / 'version.json').write_text(json.dumps(metadata, indent=2) + '\n')
    checksums = []
    for path in sorted(destination.iterdir()):
        if path.is_file() and path.name != 'SHA256SUMS':
            checksums.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}')
    (destination / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('esphome_version')
    parser.add_argument('--installer', type=Path)
    parser.add_argument('--source-ref')
    options = parser.parse_args()
    package(options.build, options.destination, options.esphome_version)
    if options.installer:
        if not options.source_ref:
            parser.error('--installer requires --source-ref')
        html = options.installer.read_text().replace('__SOURCE_REF__', quote(options.source_ref, safe=''))
        (options.destination.parent / 'index.html').write_text(html)
