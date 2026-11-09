#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and package the extraction-only helper from a configured Windows MinGW Lichii build.

This does not run an emulator. Run only when that Lichii build is idle.
The temporary CMake hook is removed in finally; source files are not patched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys
import zipfile

HERE = Path(__file__).resolve().parent

def command(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)

def output(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lichii-source', required=True, type=Path)
    parser.add_argument('--lichii-build', required=True, type=Path)
    parser.add_argument('--out', type=Path, default=HERE / 'bin')
    parser.add_argument('--jobs', type=int, default=12)
    parser.add_argument('--runtime-dir', action='append', type=Path, default=[])
    args = parser.parse_args()
    source, build, out = args.lichii_source.absolute(), args.lichii_build.absolute(), args.out.resolve()
    if os.name != 'nt': parser.error('this packaging recipe currently supports Windows MinGW builds')
    if out == source.resolve() or source.resolve() in out.parents or out == build.resolve() or build.resolve() in out.parents:
        parser.error('output must be outside the Lichii source/build tree')
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        if '=' in line and ':' in line.split('=', 1)[0]:
            key, value = line.split('=', 1); cache[key.split(':', 1)[0]] = value
    if cache.get('CMAKE_PROJECT_yuzu_INCLUDE'):
        parser.error('the configured build already has a project hook; remove it before using this recipe')
    configured_source = Path(cache['CMAKE_HOME_DIRECTORY'])
    if configured_source.resolve() != source.resolve():
        parser.error('the configured build belongs to a different Lichii source tree')
    source = configured_source
    build = Path(cache.get('CMAKE_CACHEFILE_DIR', str(build)))
    compiler = Path(cache['CMAKE_CXX_COMPILER'])
    if not compiler.is_absolute():
        compiler = Path(shutil.which(str(compiler)) or str(compiler))
    compiler_bin = compiler.parent.resolve()
    cmake = Path(cache.get('CMAKE_COMMAND', compiler_bin / 'cmake.exe'))
    objdump = compiler_bin / 'objdump.exe'
    git = shutil.which('git')
    if not git: parser.error('Git is required for corresponding-source packaging')
    gitargs = [git, '-c', 'safe.directory=*', '-C', source]
    if output(gitargs + ['status', '--porcelain', '--untracked-files=no']):
        parser.error('Lichii source has uncommitted tracked changes; commit them so the source archive matches the build')
    commit = output(gitargs + ['rev-parse', 'HEAD'])
    env = os.environ.copy(); env['PATH'] = str(compiler_bin) + os.pathsep + env.get('PATH', '')
    try:
        command([cmake, '-S', source, '-B', build,
                 '-DCMAKE_PROJECT_yuzu_INCLUDE=' + str(HERE / 'helper.cmake'),
                 '-DFEFW_ARCHIVE_SOURCE=' + str(HERE / 'archive_main.cpp'),
                 '-DFEFW_ARCHIVE_PYTHON=' + sys.executable], env=env)
        command([cmake, '--build', build, '--target', 'fefw-archive', '--parallel', max(1, args.jobs)], env=env)
    finally:
        command([cmake, '-S', source, '-B', build, '-U', 'CMAKE_PROJECT_yuzu_INCLUDE', '-U', 'FEFW_ARCHIVE_SOURCE', '-U', 'FEFW_ARCHIVE_PYTHON'], env=env)
    out.mkdir(parents=True, exist_ok=True)
    exe = build / 'bin/fefw-archive.exe'
    shutil.copy2(exe, out / exe.name)
    search = [build / 'bin', compiler_bin, *[d.resolve() for d in args.runtime_dir]]
    pending, seen = [out / exe.name], set()
    system = Path(os.environ.get('WINDIR', r'C:\Windows')) / 'System32'
    while pending:
        file = pending.pop()
        text = output([objdump, '-p', file])
        for dll in re.findall(r'DLL Name:\s*(\S+)', text):
            if dll.lower() in seen or dll.lower().startswith(('api-ms-', 'ext-ms-')): continue
            seen.add(dll.lower())
            found = next((d / dll for d in search if (d / dll).is_file()), None)
            if found is None:
                if (system / dll).is_file(): continue
                raise RuntimeError('Missing runtime DLL: ' + dll)
            target = out / dll; shutil.copy2(found, target); pending.append(target)
    license_dir = compiler_bin.parent / 'share/licenses'
    shutil.copytree(license_dir, out / 'licenses/toolchain', dirs_exist_ok=True)
    # Preserve bundled-header/static-library notices as well as runtime DLL notices.
    for dependency in (source / '.cache/cpm').glob('*/*'):
        if not dependency.is_dir(): continue
        for pattern in ('LICENSE*', 'COPYING*', 'NOTICE*'):
            for notice in dependency.glob(pattern):
                if notice.is_file():
                    destination = out / 'licenses/bundled' / dependency.parent.name / dependency.name
                    destination.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(notice, destination / notice.name)
    # LZ4 is BSD-2-Clause; retain its upstream licence supplied with this recipe.
    shutil.copy2(HERE / 'LZ4-LICENSE.txt', out / 'licenses/LZ4-LICENSE.txt')
    shutil.copy2(source / 'LICENSE.txt', out / 'licenses/Lichii-LICENSE.txt')
    shutil.copy2(HERE / 'README.md', out / 'README.md')
    # Include the exact tracked Lichii source and this helper, not any game dump/build output.
    with tempfile.TemporaryDirectory(prefix='fefw-archive-source-') as scratch:
        archived = Path(scratch) / 'lichii-source.zip'
        command(gitargs + ['archive', '--format=zip', '--output=' + str(archived), 'HEAD'])
        with zipfile.ZipFile(out / 'corresponding-source.zip', 'w', zipfile.ZIP_DEFLATED) as z:
            z.write(archived, 'lichii-source.zip')
            for name in ('archive_main.cpp', 'helper.cmake', 'prepare_reader.py', 'test_prepare_reader.py', 'build_helper.py', 'README.md', 'LZ4-LICENSE.txt'):
                z.write(HERE / name, 'helper/' + name)
    info = {'backend': 'Lichii', 'protocol': 1, 'repository': 'https://github.com/zortos293/Lichii',
            'lichii_commit': commit, 'sha256': hashlib.sha256((out / exe.name).read_bytes()).hexdigest(),
            'runtime_dlls': sorted(p.name for p in out.glob('*.dll'))}
    (out / 'build-info.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    command([out / exe.name, '--version'], env={**env, 'PATH': str(out) + os.pathsep + str(system)})
    print('Archive helper ready:', out)

if __name__ == '__main__': main()
