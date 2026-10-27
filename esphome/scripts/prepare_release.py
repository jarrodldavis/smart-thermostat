#!/usr/bin/env python3
"""Run BEFORE committing/tagging, so adopted configurations use matching metadata."""
import argparse
from pathlib import Path
import re

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', help='esphome-vMAJOR.MINOR.PATCH')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'esphome-v\d+\.\d+\.\d+', args.tag):
        parser.error('Expected esphome-vMAJOR.MINOR.PATCH')
    path = Path(__file__).resolve().parents[1] / 'smart-thermostat.yaml'
    original = path.read_text()
    updated = re.sub(r'^  project_version: .*$', f'  project_version: {args.tag[9:]}', original, flags=re.M)
    updated = re.sub(r'^  package_ref: .*$', f'  package_ref: {args.tag}', updated, flags=re.M)
    if args.check:
        if original != updated:
            parser.error('Release metadata does not match the tag; run prepare_release.py before tagging')
    else:
        path.write_text(updated)
