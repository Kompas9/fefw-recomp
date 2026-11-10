# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run game build of the release package: turns the player's own dump into fefw.exe on their PC.

    python -I tools/build/fefw_build.py --exefs <dump>/updates/<v>/exefs --game <install folder> [--jobs N] [--keep]
    python -I tools/build/fefw_build.py --check --game <install folder>

The package ships no game code. This script
  1. verifies the dump's ExeFS against dump_hashes.json (the supported build: sha256 of every module + main's build ID) and writes
     <game>/exefs: the same NSOs with main uncompressed (the console's main is ZBIC-compressed; tools/build/nso_unpack.py),
  2. lifts the six NSOs with the bundled lifter (recomp/nxrecomp) into <game>/build/lift,
  3. compiles the generated C++ with the bundled Zig (zig c++, mingw target) into <game>/build/obj, in parallel,
  4. links fefw.exe = sdk/fefw_stub.cpp + the lifted objects + sdk/fefw_rt.lib (the runtime, fefw_rt.dll in the package),
  5. deletes the generated sources and objects (unless --keep) and records the result in <game>/build/done.json.

The build stamp (state.json, done.json) is the dump (main build ID, module hashes) plus tool_stamp(): everything in this
package that goes into fefw.exe (lifter sources, main.sites, compile flags, the SDK files with fefw_rt.lib). A release that
changes any of them makes an older stamp stale: Set up then lifts, compiles and links again from scratch (the old fefw.exe
stays until the new one is linked over it). --check compares <game>/build/done.json with this package's tools only (no
dump check, well under a second) and prints one JSON line {"current": bool, "reason": text}; exit 0 current, 3 not.

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(HERE, 'recomp')
SDK = os.path.join(HERE, 'sdk')
WINDOWS = os.name == 'nt'
# the same Windows target from either host: on Linux the launcher starts fefw.exe through Wine/Proton
ZIG = os.path.join(HERE, 'zig', 'zig.exe' if WINDOWS else 'zig')
FORMAT = 2          # bump when the build layout or the stamp changes: older builds (partial or done) are then started over
# the lifter as game-build/scripts/make-release.ps1 ships it: paths under RECOMP, the same in the zip (tools/build/recomp/...)
# and in the repo (recomp/...)
LIFTER = ['nxrecomp', 'include/nxrecomp', 'games/fefw/CMakeLists.txt', 'games/fefw/config.py']
SITES = 'games/fefw/main.sites'
# what the link takes from sdk/ besides the lifted objects and fefw_rt.lib (stamped on its own, as 'runtime')
SDK_FILES = ['fefw_stub.cpp', 'fefw_rt_api.h', 'fefw.rc', 'fefw.manifest', 'fefw.ico']

# name, NSO file, guest base, NxModule.index, translation units (as recomp/games/fefw/CMakeLists.txt)
MODULES = [
    ('main', 'main', 0x7100000000, 0, 153),
    ('sdk', 'sdk', 0x7500000000, 1, 50),
    ('subsdk0', 'subsdk0', 0x7600000000, 2, 5),
    ('subsdk1', 'subsdk1', 0x7640000000, 3, 14),
    ('subsdk2', 'subsdk2', 0x7680000000, 4, 3),
    ('rtld', 'rtld', 0x7700000000, 5, 1),
]
EXEFS_FILES = ['main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'rtld', 'main.npdm']
# recomp/cmake/NxRecomp.cmake flags for the lifted code, in clang (not clang-cl) syntax
# plus recomp/games/fefw/CMakeLists.txt's -mfma and NX_INLINE_INDIRECT
CXXFLAGS = ['-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16+fma', '-std=c++20', '-O1', '-w', '-fno-exceptions', '-fno-rtti',
            '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno', '-fno-threadsafe-statics', '-DNX_INLINE_INDIRECT']
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def progress(phase, done, total, text):
    say('progress', phase, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tree_hash(root, rels):
    """One sha256 over the files under root/rel for each rel (a file or a folder, walked in sorted order): each file's path
    relative to root and its sha256. Python's bytecode caches are left out (the lifter's own runs write them); a missing
    path counts as missing, so the hash still compares."""
    h = hashlib.sha256()
    for rel in rels:
        top = os.path.join(root, *rel.split('/'))
        if os.path.isfile(top):
            files = [rel]
        elif os.path.isdir(top):
            files = []
            for d, dirs, names in os.walk(top):
                dirs[:] = [x for x in dirs if x != '__pycache__']
                files += [os.path.relpath(os.path.join(d, n), root).replace('\\', '/') for n in names if not n.endswith('.pyc')]
        else:
            h.update(('%s\0missing\0' % rel).encode())
            continue
        for f in sorted(files):
            h.update(('%s\0%s\0' % (f, sha256(os.path.join(root, *f.split('/'))))).encode())
    return h.hexdigest()


# The launcher's SHADER_TRANSLATOR_VERSION (launcher/src-tauri/src/setup.rs): keep the two equal.
SHADER_TRANSLATOR_VERSION = 2


def tool_stamp():
    """The part of the build stamp this package decides: a release that changes any of it rebuilds fefw.exe."""
    return {'lifter': tree_hash(RECOMP, LIFTER), 'sites': tree_hash(RECOMP, [SITES]), 'sdk': tree_hash(SDK, SDK_FILES),
            'runtime': sha256(os.path.join(SDK, 'fefw_rt.lib')), 'cxxflags': CXXFLAGS, 'modules': [list(m) for m in MODULES]}


def check(game):
    """Whether fefw.exe and <game>/build/done.json were built with this package's tools: (current, reason)."""
    if not os.path.isfile(os.path.join(game, 'fefw.exe')):
        return False, 'fefw.exe is not built yet'
    done = read_json(os.path.join(game, 'build', 'done.json'))
    if not isinstance(done, dict):
        return False, 'fefw.exe has no finished build record'
    if done.get('format') != FORMAT or not isinstance(done.get('tools'), dict):
        return False, 'fefw.exe was built by an older release'
    want = tool_stamp()
    changed = [k for k in want if done['tools'].get(k) != want[k]]
    if changed:
        return False, 'this release changes ' + ', '.join(changed)
    shaders = read_json(os.path.join(game, 'shader_cache', 'setup.json'))
    if isinstance(shaders, dict) and shaders.get('translator_version') != SHADER_TRANSLATOR_VERSION:
        return False, 'this release changes the shader translator (shaders are translated again)'
    return True, 'fefw.exe matches this release'


def read_json(path):
    """The JSON in path, or None if it is missing or unreadable (a file cut short by a crash or power loss starts over)."""
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json(path, value):
    """Write through a temporary file and rename, so a stop mid-write never leaves half a file behind."""
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, indent=1)
    os.replace(tmp, path)


def verify(exefs):
    want = json.load(open(os.path.join(HERE, 'dump_hashes.json'), encoding='utf-8'))
    missing = [n for n in want['sha256'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        raise Fail('The game files folder %s is missing %s. Pick the exefs of a complete dump.' % (exefs, ', '.join(missing)))
    with open(os.path.join(exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise Fail('exefs/main is not a decrypted NSO. Extract the game files with your own keys first (decrypted exefs and romfs).')
    bid = head[0x40:0x54].hex()
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). This release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    names = list(want['sha256'])
    for i, n in enumerate(names):
        progress('verify', i, len(names), 'Checking your game files: %s' % n)
        got = sha256(os.path.join(exefs, n))
        if n == 'main' and got == want.get('main_plain_sha256'):
            continue                     # a dump tool that already wrote main uncompressed
        if got != want['sha256'][n]:
            raise Fail('exefs/%s does not match the supported %s dump (sha256 %s). Re-dump the game, or check that the update %s is installed.'
                       % (n, want['version'], got, want['version']))
    progress('verify', len(names), len(names), 'Game files match %s' % want['version'])
    return want


def write_exefs(exefs, out):
    """<game>/exefs: the dump's NSOs, main uncompressed (the lifter and the runtime read plain NSOs only)."""
    sys.path.insert(0, HERE)
    from nso_unpack import unpack
    os.makedirs(out, exist_ok=True)
    for n in EXEFS_FILES:
        src = os.path.join(exefs, n)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(out, n)
        if n == 'main':
            data = open(src, 'rb').read()
            flags = int.from_bytes(data[12:16], 'little')
            if flags & 0x87:
                progress('verify', 0, 1, 'Unpacking main (ZBIC)')
                data, _ = unpack(data)
            tmp = dst + '.tmp'
            with open(tmp, 'wb') as f:
                f.write(data)
            os.replace(tmp, dst)
        else:
            shutil.copyfile(src, dst)


def popen(cmd, **kw):
    if WINDOWS:
        kw['creationflags'] = CREATE_NO_WINDOW | BELOW_NORMAL
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)


class Runner:
    """Runs tool processes; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if os.path.exists(self.cancel_file):
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or os.path.exists(self.cancel_file):
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, env=None, cwd=None, on_line=None):
        self.check()
        p = popen(cmd, env=env, cwd=cwd)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        return rc, '\n'.join(tail)


def lift(runner, exefs, gen, jobs):
    """One lifter process per module (the lifter itself runs `jobs` worker processes)."""
    sites = os.path.join(RECOMP, 'games', 'fefw', 'main.sites')
    for k, (name, nso, base, index, shards) in enumerate(MODULES):
        out = os.path.join(gen, name)
        marker = os.path.join(out, '.lifted')
        if os.path.isfile(marker):
            continue
        progress('lift', k, len(MODULES), 'Translating the game code: %s (%d of %d)' % (name, k + 1, len(MODULES)))
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
        args = ['lift', os.path.join(exefs, nso), out, '--name', name, '--base', hex(base), '--shards', str(shards),
                '--jobs', str(jobs), '--game', 'fefw', '--index', str(index)]
        if name == 'main':
            args += ['--sites', sites]
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--lift-one', json.dumps(args)]
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
        open(marker, 'w').write('ok\n')
    progress('lift', len(MODULES), len(MODULES), 'Game code translated')


def free_memory():
    """Available physical memory in bytes, or None."""
    if not WINDOWS:
        try:
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            pass
        return None
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [(n, ctypes.c_uint64) for n in
                        ('total', 'avail', 'pagetotal', 'pageavail', 'virttotal', 'virtavail', 'extavail')]
        m = MS(len=ctypes.sizeof(MS))
        return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
    except (AttributeError, OSError):
        return None


def compile_all(runner, gen, obj, jobs, env):
    units = []
    for name, _, _, _, shards in MODULES:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not os.path.isfile(o):
            todo.append((name, src, o))
    done = total - len(todo)
    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    for name, _, _, _, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)
    # biggest first, so the long ones do not end up last on a single core
    todo.sort(key=lambda t: -os.path.getsize(t[1]))

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        cmd = [ZIG, 'c++'] + CXXFLAGS + ['-I', os.path.join(RECOMP, 'include'), '-I', os.path.dirname(src), '-c', src, '-o', tmp]
        rc, tail = runner.run(cmd, env=env)
        if rc != 0:
            return 'Compiling %s/%s failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail)
        os.replace(tmp, o)
        return None

    # a big unit takes a GB or two in clang; with too many at once memory runs out and clang dies on every unit with
    # exit 541541187 (0x20474343: an uncaught C++ exception, std::bad_alloc)
    # ponytail: 2 GiB per job is a guess from the unit sizes; measure if machines with plenty of RAM get capped
    avail = free_memory()
    if avail:
        jobs = max(1, min(jobs, avail // (2 << 30)))
    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t): t for t in todo}
        try:
            while pending:
                fin, _ = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    t = pending.pop(f)
                    if f.result():
                        failed.append(t)
                        continue
                    done += 1
                    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise
    for t in failed:                     # again one at a time with all the memory: only a failure here is real
        err = one(t)
        if err:
            raise Fail(err)
        done += 1
        progress('compile', done, total, 'Compiling: %d of %d' % (done, total))


def clear_high_entropy_va(exe):
    """fefw.exe keeps host allocations low (the guest ranges at 0x71_0000_0000 must stay free): no high-entropy ASLR,
    as /HIGHENTROPYVA:NO does for the clang-cl build. zig's linker has no flag for it, so clear the PE bit."""
    with open(exe, 'r+b') as f:
        f.seek(0x3c)
        pe = int.from_bytes(f.read(4), 'little')
        f.seek(pe + 24)
        if f.read(2) != b'\x0b\x02':
            raise Fail('fefw.exe is not a PE32+ image')
        off = pe + 24 + 70
        f.seek(off)
        dc = int.from_bytes(f.read(2), 'little') & ~0x20
        f.seek(off)
        f.write(dc.to_bytes(2, 'little'))


def link(runner, obj, game, env):
    progress('link', 0, 1, 'Linking fefw.exe')
    libs = []
    for name, _, _, _, _ in MODULES:
        a = os.path.join(obj, 'fefw_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        # UTF-8, not the ANSI code page: zig ar reads the response file as UTF-8, so a game folder path with non-ASCII
        # characters (an accented user name) failed the archive step.
        with open(rsp, 'w', encoding='utf-8') as f:
            f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
        rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
        libs.append(a)
    tmp = os.path.join(obj, 'fefw.exe')
    cmd = [ZIG, 'c++', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK,
           os.path.join(SDK, 'fefw_stub.cpp'), os.path.join(SDK, 'fefw.rc')] + libs + [os.path.join(SDK, 'fefw_rt.lib'),
           '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
    rc, tail = runner.run(cmd, env=env, cwd=SDK)
    if rc != 0:
        raise Fail('Linking fefw.exe failed (exit %d):\n%s' % (rc, tail))
    clear_high_entropy_va(tmp)
    dst = os.path.join(game, 'fefw.exe')
    os.replace(tmp, dst)
    progress('link', 1, 1, 'fefw.exe built')
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs')
    ap.add_argument('--game')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--check', action='store_true', help='only say whether <game>/build/done.json matches these tools')
    ap.add_argument('--lift-one', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.lift_one:                       # child process: one lifter run (keeps the lifter's own pool out of this process)
        sys.path.insert(0, RECOMP)
        from nxrecomp.__main__ import main as nx_main
        return nx_main(json.loads(a.lift_one)) or 0
    if a.check:
        if not a.game:
            ap.error('--check needs --game')
        try:
            current, reason = check(os.path.abspath(a.game))
        except OSError as e:                 # a tool file that cannot be read: Set up then runs and names it
            current, reason = False, 'cannot read the build tools: %s' % e
        print(json.dumps({'current': current, 'reason': reason}), flush=True)
        return 0 if current else 3
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    if not WINDOWS:
        try:
            os.nice(10)                  # the BELOW_NORMAL of the Windows build: the lifter and the compilers inherit it
        except OSError:
            pass
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        if not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        want = verify(a.exefs)
        plain = os.path.join(game, 'exefs')
        write_exefs(a.exefs, plain)
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'tools': tool_stamp()}
        state_p = os.path.join(build, 'state.json')
        # another dump or another release (or no readable state): start over. Removing lift/ drops the modules' .lifted
        # markers too, so a changed lifter or main.sites lifts everything again, not only recompiles
        if read_json(state_p) != stamp:
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            write_json(state_p, stamp)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        gen, obj = os.path.join(build, 'lift'), os.path.join(build, 'obj')
        t = time.time()
        lift(runner, plain, gen, a.jobs)
        times['lift'] = round(time.time() - t, 1)
        t = time.time()
        compile_all(runner, gen, obj, a.jobs, env)
        times['compile'] = round(time.time() - t, 1)
        t = time.time()
        exe = link(runner, obj, game, env)
        times['link'] = round(time.time() - t, 1)
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj', 'zig-cache'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        done = dict(stamp, exe_sha256=sha256(exe), seconds=times, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        write_json(os.path.join(build, 'done.json'), done)
        progress('finish', 1, 1, 'fefw.exe is ready (%d s)' % (time.time() - t0))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
