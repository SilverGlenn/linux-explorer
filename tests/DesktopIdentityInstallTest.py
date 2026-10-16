#!/usr/bin/env python3
"""Exercise desktop-entry installer templates without installing application binaries."""

import os
from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1]
prefix = '/identity # % Ω " prefix'
identities = ('linux-explorer-win7-dialog', 'linux-explorer-win7-chooser-dialog')


def escaped(value):
    return value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%')


# Browser-first and service-only installations must retain both hidden IDs.
# Portal also prefers an installed chooser over its own executable.
for installer, programs, expected in (
    ('DesktopEntries', ('explorer-win7',), 'explorer-win7'),
    ('ChooserService', (), 'explorer-win7-chooser'),
    ('ChooserService', ('explorer-win7',), 'explorer-win7'),
    ('PortalService', (), 'explorer-win7-portal'),
    ('PortalService', ('explorer-win7-chooser',), 'explorer-win7-chooser'),
    ('PortalService', ('explorer-win7', 'explorer-win7-chooser'), 'explorer-win7'),
):
    with tempfile.TemporaryDirectory(prefix='explorer-desktop-identities-') as temporary:
        work = Path(temporary)
        staged = work / 'stage' / prefix.lstrip('/')
        applications = staged / 'share/applications'
        applications.mkdir(parents=True)
        (staged / 'bin').mkdir()
        for program in programs:
            (staged / 'bin' / program).touch()
        script = work / 'installer.cmake'
        template = (source / f'cmake/Install{installer}.cmake.in').read_text()
        script.write_text(template.replace('@PROJECT_SOURCE_DIR@', str(source))
                         .replace('@CMAKE_CURRENT_SOURCE_DIR@', str(source)))
        subprocess.run(['cmake', f'-DCMAKE_INSTALL_PREFIX={prefix}', '-P', str(script)],
                       env={**os.environ, 'DESTDIR': str(work / 'stage')}, check=True)
        arguments = ' %U' if expected == 'explorer-win7' else ''
        expected_exec = f'Exec="{escaped(prefix + "/bin/" + expected)}"{arguments}'
        for identity in identities:
            actual = (applications / f'{identity}.desktop').read_text()
            original = (source / f'{identity}.desktop').read_text()
            assert actual == original.replace('Exec=explorer-win7 %U', expected_exec), (installer, identity)
            assert f'StartupWMClass={identity}\n' in actual, (installer, identity)
            assert 'NoDisplay=true\n' in actual, (installer, identity)
            assert str(work) not in actual, (installer, identity)
        if installer == 'DesktopEntries':
            original = (source / 'linux-explorer-win7.desktop').read_text()
            actual = (applications / 'linux-explorer-win7.desktop').read_text()
            assert actual == original.replace('Exec=explorer-win7',
                                               f'Exec="{escaped(prefix + "/bin/explorer-win7")}"')

print('Browser, Chooser and Portal desktop identities, Exec escaping and DESTDIR: PASS')
