#!/usr/bin/env python3
"""Collect supplied Linux-target dependency notices offline; never fetch packages."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('destination', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
metadata = json.loads(subprocess.check_output([
    'cargo', 'metadata', '--offline', '--locked', '--features', 'native-aoa',
    '--filter-platform', 'x86_64-unknown-linux-gnu', '--format-version', '1',
], cwd=root))
records = []
missing = []
for package in sorted(metadata['packages'], key=lambda p: (p['name'], p['version'])):
    if package['name'] == 'cordgeist':
        continue
    source = Path(package['manifest_path']).parent.resolve()
    # Pinned sibling Rust bindings inherit the vendored upstream distribution's
    # root license/notice files; their manifests declare MIT.
    upstream = root / 'vendor' / 'libaoahid'
    if package['name'] in ('aoahid', 'aoahid-sys') and source.is_relative_to(upstream):
        source = upstream
    name = package['name'] + '-' + package['version']
    files = set()
    for path in source.rglob('*'):
        if path.is_file() and path.name.upper().startswith((
            'LICENSE', 'LICENCE', 'NOTICE', 'COPYING', 'COPYRIGHT',
        )):
            files.add(path)
    if (source / 'THIRD_PARTY_NOTICES.md').is_file():
        files.add(source / 'THIRD_PARTY_NOTICES.md')
    if package.get('license_file'):
        files.add(source / package['license_file'])
    if not files or not package.get('license') and not package.get('license_file'):
        missing.append(name)
        continue
    supplied = []
    for path in sorted(files):
        resolved = path.resolve()
        if not resolved.is_relative_to(source) or not resolved.is_file():
            raise SystemExit('Invalid notice path in ' + name)
        relative = path.relative_to(source)
        target = args.destination / name / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(resolved, target)
        supplied.append(str(relative))
    records.append({'name': package['name'], 'version': package['version'],
                    'license': package.get('license'), 'repository': package.get('repository'),
                    'suppliedFiles': supplied})
if missing:
    raise SystemExit('Missing supplied notice material: ' + ', '.join(missing))
args.destination.mkdir(parents=True, exist_ok=True)
(args.destination / 'DEPENDENCIES.json').write_text(json.dumps(records, indent=2) + '\n')
print('Collected supplied notices for', len(records), 'Linux-target dependency packages')
