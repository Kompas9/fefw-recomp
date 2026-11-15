#!/usr/bin/env python3
"""Build the Ampere (NVN2) shader corpus of a Switch 2 game from its romfs, main and ExeFS.

Sources:
  * KTGL games (Pokopia): romfs motor_asset_re/<platform>/*.fdata and data/*.file: IDRK entries (see
    .weaver/ktgl_rdb.py); entries whose inflated data starts with '_S2G' (G2S shader container, type KTID 0x7bcd279f) or
    '_DOK' (system resources, type KTID 0x20a6a0bb) are searched for GLSLC outputs.
  * nn::gfx games (Fire Emblem Fortune's Weave): every romfs file named *.bfsha, *.ptcl, *.arc, *.sarc, *.bnsh (each
    optionally zstd-compressed, *.zst) is searched for BNSH shader files (docs/AMPERE.md section 8): BFSHA shader archives,
    VFXB effect files, SARC layout archives.
  * main (decompressed ELF/NSO segment dump, or the ExeFS 'main' NSO itself, decompressed here when it is LZ4): searched
    for GLSLC outputs and BNSH files as is. A ZBIC-compressed main (NSO flag bit 7) cannot be read here: pass the
    decompressed NSO the recompiler works from.
  * --exefs <dir>: the other NSOs (sdk, subsdk*) are searched for CUDA fatbins; every kernel of every sm_8x cubin becomes
    a compute block (DLSS runs as CUDA kernels in subsdk0). Its "control section" is a CUDA kernel descriptor written here
    (magic 'CUDA', see cuda_descriptor) instead of an NVN one.

A GLSLC output (NVN2 layout, see docs/AMPERE.md): u32 magic 0x19866891, +0x5c u32 output length, +0x64 u32 section count,
section header i at +0xa8 + 0x98*i = {u32 size, u32 offset from output, u32 type}; type 0 (GPU code) adds
+0x30 control offset, +0x34 code offset (both from the section data), +0x38 code length, +0x3c control length.

Output (--out), content-addressed by the first 16 hex of SHA-1 of the bytes: code/<h>.bin (code area: SPH v4, then
SASS; for CUDA the kernel's .text), ctrl/<h>.bin (control sections or CUDA descriptors), glslc/<h>.bin (whole GLSLC
outputs; BNSH and CUDA blocks have none, '-'); manifest.tsv (one row per occurrence with all three hashes), unique.tsv
(one row per distinct code block, with its first occurrence's ctrl/glslc), programs.tsv (BNSH programs: the code hash of
each stage), cuda_kernels.tsv (kernel name, module and launch data per CUDA block), summary.txt.
Deterministic: files and entries are visited in sorted order; "first occurrence" is in that order.
zstd: Python 3.14's compression.zstd, else the zstandard package, else an external zstd or 7-Zip (--zstd <exe>, or
zstd/7z on PATH).
--programs-bin <file> also writes the runtime's program list: the distinct vertex + fragment (+ geometry) pairs of programs.tsv as "MPRPRG01", a u32
count, a u32 0 and three u64 code hashes per program. The game's shader preparation (fefw.exe --prepare-shaders) compiles those ahead of play.
Usage: python -I extract_corpus.py --romfs <romfs> [--main <main>] [--exefs <dir>] --out <dir> [--jobs 4] [--zstd <exe>] [--programs-bin <file>]
"""
import argparse, collections, hashlib, mmap, os, shutil, struct, subprocess, sys, zlib
from concurrent.futures import ProcessPoolExecutor

IDRK = b'IDRK0000'
GLSLC_MAGIC = b'\x91\x68\x86\x19'
CTRL_MAGIC = 0x98761234
CTRL_STAGES = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs'}
BNSH_MAGIC = b'BNSH\0\0\0\0'
ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
BNSH_FILES = ('.bfsha', '.ptcl', '.arc', '.sarc', '.bnsh')
FATBIN_MAGIC = b'\x50\xed\x55\xba'
CUDA_MAGIC = 0x41445543   # 'CUDA'


def low_priority():
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL


def entries(d):
    o, n = 0, len(d)
    while o + 0x30 <= n:
        if d[o:o + 8] != IDRK:
            o = d.find(IDRK, o)
            if o < 0:
                return
        esz, csz, usz = struct.unpack_from('<QQQ', d, o + 8)
        _, fk, tk, fl = struct.unpack_from('<4I', d, o + 0x20)
        if esz < 0x30 or o + esz > n:
            return
        yield o, o + esz - csz, o + esz, usz, fk, tk, fl
        o += esz


def inflate(d, a, b, flags, head=None):
    if not flags & 0x500000:
        return bytes(d[a:min(b, a + head) if head else b])
    new, lz4b = flags & 0x400000, (flags & 0x500000) == 0x500000
    out, got = [], 0
    while a + 4 <= b:
        if new:
            cs = struct.unpack_from('<H', d, a)[0]; a += 10
        else:
            cs = struct.unpack_from('<I', d, a)[0]; a += 4
        if not cs:
            break
        if lz4b:
            import lz4.block
            r = lz4.block.decompress(bytes(d[a:a + cs]), uncompressed_size=0x4000)
        else:
            r = zlib.decompress(bytes(d[a:a + cs]))
        out.append(r); got += len(r); a += cs
        if head and got >= head:
            break
    r = b''.join(out)
    return r[:head] if head else r


def stage_of(ctrl):
    """NVN stage from control +0x56 (u16): 0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs. Agrees with the SPH v4 shader type of
    every graphics block; one compute block's first instruction word happens to look like an SPH, so the SPH is not used."""
    return CTRL_STAGES.get(struct.unpack_from('<H', ctrl, 0x56)[0], '?') if len(ctrl) >= 0x58 else '?'


def glslc_outputs(d):
    """-> [(output offset, output bytes, [(section index, ctrl, code)])]"""
    res = []
    o = d.find(GLSLC_MAGIC)
    while o >= 0:
        ok = False
        if o + 0xa8 <= len(d):
            size, = struct.unpack_from('<I', d, o + 0x5c)
            nsec, = struct.unpack_from('<I', d, o + 0x64)
            if 0xa8 < size <= len(d) - o and 0 < nsec <= 16 and 0xa8 + 0x98 * nsec <= size:
                secs = []
                for i in range(nsec):
                    s = o + 0xa8 + 0x98 * i
                    ssz, soff, typ = struct.unpack_from('<3I', d, s)
                    if typ != 0:
                        continue
                    co, ko, kn, cn = struct.unpack_from('<4I', d, s + 0x30)
                    if soff + ssz > size or co + cn > ssz or ko + kn > ssz:
                        secs = None; break
                    base = o + soff
                    ctrl, code = bytes(d[base + co:base + co + cn]), bytes(d[base + ko:base + ko + kn])
                    if len(ctrl) < 8 or struct.unpack_from('<I', ctrl)[0] != CTRL_MAGIC:
                        secs = None; break
                    secs.append((i, ctrl, code))
                if secs:
                    res.append((o, bytes(d[o:o + size]), secs))
                    ok = True
                    o = d.find(GLSLC_MAGIC, o + size)
                    continue
        o = d.find(GLSLC_MAGIC, o + 4)
    return res


# ---- BNSH (nn::gfx shader file) ---------------------------------------------------------------------------------------
# BinaryFileHeader: "BNSH\0\0\0\0", u32 version, u16 BOM 0xfeff, u8 alignment, u8 address size, u32 name offset, u16 flags,
# u16 first block (the 'grsc' container), u32 relocation table, u32 file size. Pointers are u64 offsets from the BNSH start.
# Container (grsc block): +0x1c u32 variation count, +0x20 variations (0x40 each: source, intermediate, binary program,
# parent). Binary program: +0x8 six ShaderCode pointers (vertex, hull, domain, geometry, pixel, compute).
# ShaderCode (NVN): +0x8 control, +0x10 code, +0x18 u32 code size, +0x1c u32 control size.

def bnsh_files(d):
    o = d.find(BNSH_MAGIC)
    while o >= 0:
        if o + 0x60 <= len(d) and struct.unpack_from('<H', d, o + 0xc)[0] == 0xfeff:
            size, = struct.unpack_from('<I', d, o + 0x1c)
            first, = struct.unpack_from('<H', d, o + 0x16)
            if 0x60 <= size <= len(d) - o and d[o + first:o + first + 4] == b'grsc':
                yield o, size, first
        o = d.find(BNSH_MAGIC, o + 8)


def bnsh_programs(d, base, size, first):
    """-> [(variation, [(slot, ctrl, code)])] for every binary program; malformed stages are left out."""
    def u32(a): return struct.unpack_from('<I', d, a)[0]
    def u64(a): return struct.unpack_from('<Q', d, a)[0]
    end = base + size
    c = base + first
    nvar, pvar = u32(c + 0x1c), u64(c + 0x20)
    res = []
    for v in range(nvar):
        va = base + pvar + 0x40 * v
        if va + 0x40 > end:
            break
        pbin = u64(va + 0x10)
        if not pbin or base + pbin + 0x38 > end:
            continue
        stages = []
        for slot in range(6):
            cp = u64(base + pbin + 8 + 8 * slot)
            if not cp or base + cp + 0x20 > end:
                continue
            ca = base + cp
            pctrl, pcode, ksz, csz = u64(ca + 8), u64(ca + 0x10), u32(ca + 0x18), u32(ca + 0x1c)
            if base + pctrl + csz > end or base + pcode + ksz > end or csz < 0xd8:
                continue
            ctrl = bytes(d[base + pctrl:base + pctrl + csz])
            if struct.unpack_from('<I', ctrl)[0] != CTRL_MAGIC or struct.unpack_from('<I', ctrl, 0x44)[0] != ksz:
                continue
            stages.append((slot, ctrl, bytes(d[base + pcode:base + pcode + ksz])))
        res.append((v, stages))
    return res


# ---- zstd ---------------------------------------------------------------------------------------------------------------

ZSTD_TOOL = None


def find_zstd_tool(given):
    if given:
        return given
    for exe in ('zstd', '7z', '7za'):
        p = shutil.which(exe)
        if p:
            return p
    scoop = os.path.expandvars(r'%USERPROFILE%\scoop\apps\7zip\current\7z.exe')
    return scoop if os.path.exists(scoop) else None


def unzstd(path, data):
    try:
        from compression import zstd  # Python 3.14
        return zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)
    except ImportError:
        pass
    if not ZSTD_TOOL:
        raise RuntimeError('zstd: no compression.zstd (Python 3.14), zstandard package or zstd/7z executable (--zstd)')
    tool = os.path.basename(ZSTD_TOOL).lower()
    cmd = [ZSTD_TOOL, 'e', '-so', path] if tool.startswith('7z') else [ZSTD_TOOL, '-dcq', path]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f'{ZSTD_TOOL} failed on {path}: {r.stderr[:200]!r}')
    return r.stdout


# ---- CUDA fatbins and cubins --------------------------------------------------------------------------------------------
# Fatbin: u32 magic 0xba55ed50, u16 version 1, u16 header size 0x10, u64 size of the entries; entry: u16 kind (2 = ELF),
# u16, u32 header size, u64 payload size, ..., +0x1c u32 SM (88 = sm_88). Cubin: ELF64 with .text.<kernel> (SASS),
# .nv.info.<kernel> (EIATTR records), .nv.shared.<kernel> (static shared memory), .nv.constant0.<kernel> (bank 0: driver
# words, then the parameters), .nv.constant4 (module bank 4: 64-bit addresses of .nv.global/.nv.global.init, by relocation).

def fatbin_cubins(img):
    o = img.find(FATBIN_MAGIC)
    while o >= 0:
        ver, hs, fs = struct.unpack_from('<HHQ', img, o + 4)
        if ver == 1 and hs == 0x10 and 0 < fs <= len(img) - o - hs:
            e = o + hs
            while e + 0x20 <= o + hs + fs:
                kind, _, ehs = struct.unpack_from('<HHI', img, e)
                psz, = struct.unpack_from('<Q', img, e + 8)
                sm, = struct.unpack_from('<I', img, e + 0x1c)
                if ehs < 0x20 or e + ehs + psz > o + hs + fs:
                    break
                body = img[e + ehs:e + ehs + psz]
                if kind == 2 and body[:4] == b'\x7fELF':
                    yield o, e + ehs, sm, bytes(body)
                e += ehs + psz
            o = img.find(FATBIN_MAGIC, o + hs + fs)
        else:
            o = img.find(FATBIN_MAGIC, o + 4)


def elf_sections(d):
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    secs = []
    for i in range(shnum):
        nm, typ, fl, _, off, sz, link, info, _, entsz = struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize)
        secs.append({'name': nm, 'type': typ, 'off': off, 'size': sz, 'link': link, 'info': info, 'entsz': entsz})
    stro = secs[shstrndx]['off']
    for s in secs:
        s['name'] = d[stro + s['name']:d.index(b'\0', stro + s['name'])].decode('latin1')
    return secs


def nv_info(data):
    """EIATTR records: (attribute, value) with value bytes (format 4) or int (format 3)."""
    o, out = 0, []
    while o + 4 <= len(data):
        fmt, attr = data[o], data[o + 1]
        if fmt == 4:
            n, = struct.unpack_from('<H', data, o + 2)
            out.append((attr, bytes(data[o + 4:o + 4 + n]))); o += 4 + n
        elif fmt == 3:
            out.append((attr, struct.unpack_from('<H', data, o + 2)[0])); o += 4
        elif fmt in (1, 2):
            out.append((attr, bytes(data[o + 2:o + 4]))); o += 4
        else:
            break
    return out


def cuda_descriptor(sm, text_size, attrs, shared, regs, c4):
    """The CUDA kernel descriptor the harness reads in place of an NVN control section. u32 words:
    [0] 'CUDA', [1] version 1, [2] SM, [3] text size, [4] parameter offset in bank 0, [5] parameter bytes,
    [6] static shared memory bytes, [7..9] EIATTR_MAX_THREADS x y z (0: none), [10..12] EIATTR_REQNTID x y z (0: none),
    [13] registers, [14] frame (local memory) bytes, [15] bank 4 bytes, [16] indirect branch count, then per BRX
    {u32 address, u32 target count, targets}, then the bank 4 bytes (relocated addresses read as 0)."""
    def first(a): return next((v for k, v in attrs if k == a), None)
    pc = first(0x0a)   # EIATTR_PARAM_CBANK: u32 symbol, u16 offset, u16 size
    poff, psz = struct.unpack_from('<HH', pc, 4) if isinstance(pc, bytes) and len(pc) >= 8 else (0x160, 0)
    mt = first(0x05)   # EIATTR_MAX_THREADS
    rq = first(0x10)   # EIATTR_REQNTID
    mt = struct.unpack_from('<3I', mt) if isinstance(mt, bytes) and len(mt) >= 12 else (0, 0, 0)
    rq = struct.unpack_from('<3I', rq) if isinstance(rq, bytes) and len(rq) >= 12 else (0, 0, 0)
    brx = []
    for k, v in attrs:
        if k == 0x34 and isinstance(v, bytes):   # EIATTR_INDIRECT_BRANCH_TARGETS: {u32 address, u32, u32 count, targets}
            p = 0
            while p + 12 <= len(v):
                addr, _, n = struct.unpack_from('<3I', v, p)
                brx.append((addr, list(struct.unpack_from(f'<{n}I', v, p + 12))))
                p += 12 + 4 * n
    words = [CUDA_MAGIC, 1, sm, text_size, poff, psz, shared, *mt, *rq, regs, 0, len(c4), len(brx)]
    out = struct.pack(f'<{len(words)}I', *words)
    for addr, ts in brx:
        out += struct.pack(f'<II{len(ts)}I', addr, len(ts), *ts)
    return out + c4


def cubin_kernels(elf, sm):
    """-> [(kernel name, text, descriptor, launch info dict)]"""
    secs = elf_sections(elf)
    by = {s['name']: s for s in secs}
    def data(s): return elf[s['off']:s['off'] + s['size']] if s['type'] != 8 else b''
    c4 = data(by['.nv.constant4']) if '.nv.constant4' in by else b''
    out = []
    for s in secs:
        if not s['name'].startswith('.text.'):
            continue
        k = s['name'][6:]
        attrs = nv_info(data(by['.nv.info.' + k])) if '.nv.info.' + k in by else []
        shared = by['.nv.shared.' + k]['size'] if '.nv.shared.' + k in by else 0
        regs = s['info'] >> 24
        text = data(s)
        desc = cuda_descriptor(sm, len(text), attrs, shared, regs, c4)
        w = struct.unpack_from('<16I', desc)
        out.append((k, text, desc, {'param_off': w[4], 'param_size': w[5], 'shared': shared, 'max_threads': w[7:10],
                                    'reqntid': w[10:13], 'regs': regs, 'bank4': len(c4)}))
    return out


# ---- output -------------------------------------------------------------------------------------------------------------

OUT = None


def put(sub, h, data):
    """Content-addressed write (same name = same bytes, so parallel workers cannot disagree)."""
    p = os.path.join(OUT, sub, h + '.bin')
    if not os.path.exists(p):
        t = f'{p}.{os.getpid()}.tmp'
        with open(t, 'wb') as f:
            f.write(data)
        try:
            os.replace(t, p)
        except PermissionError:
            # Windows refuses to replace a file another worker (or a virus scanner) has open. That worker wrote the same
            # block (content-addressed), so keep its copy.
            if not os.path.exists(p):
                raise
            os.remove(t)


def h16(b):
    return hashlib.sha1(b).hexdigest()[:16]


def slim(occs):
    """Write the blobs; return the rows without bytes. gb None: no GLSLC output (BNSH, CUDA)."""
    rows = []
    for name, eo, fk, tk, tag, go, si, st, ctrl, code, gb in occs:
        h, ch = h16(code), h16(ctrl)
        put('code', h, code); put('ctrl', ch, ctrl)
        gh = '-'
        if gb is not None:
            gh = h16(gb)
            put('glslc', gh, gb)
        rows.append((name, eo, fk, tk, tag, go, si, st, len(ctrl), len(code), h, ch, gh))
    return rows


def bnsh_rows(name, d):
    """Occurrence rows and program rows of every BNSH in d."""
    occs, progs = [], []
    for base, size, first in bnsh_files(d):
        for v, stages in bnsh_programs(d, base, size, first):
            occs += [(name, base, 0, 0, 'BNSH', v, slot, stage_of(ctrl), ctrl, code, None) for slot, ctrl, code in stages]
            progs.append((name, base, v, ' '.join(f'{stage_of(c)}={h16(k)}' for _, c, k in stages)))
    return slim(occs), progs


def scan_file(path, out, rel):
    global OUT
    OUT = out
    low_priority()
    rows, progs = [], []
    name = os.path.basename(path)
    low = name.lower()
    if low.endswith(('.fdata', '.file')):
        with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
            for o, a, b, usz, fk, tk, fl in entries(d):
                tag = inflate(d, a, b, fl, 8)
                if tag[:4] not in (b'_S2G', b'_DOK'):
                    continue
                data = inflate(d, a, b, fl)
                rows += slim([(name, o, fk, tk, tag.decode('latin1'), go, si, stage_of(ctrl), ctrl, code, gb)
                              for go, gb, secs in glslc_outputs(data) for si, ctrl, code in secs])
        return rows, progs
    data = open(path, 'rb').read()
    if data[:4] == ZSTD_MAGIC:
        data = unzstd(path, data)
    return bnsh_rows(rel, data)


def nso_image(d):
    """A compressed NSO (the ExeFS 'main' as dumped) -> its memory image: .text, .rodata, .data at their memory offsets
    (header +0x10 three {file off, mem off, size, align}, +0x60 compressed sizes, flag bits 0..2 = segment LZ4-compressed,
    bit 7 = ZBIC instead of LZ4, which is not supported here)."""
    flags, = struct.unpack_from('<I', d, 0xc)
    if flags & 0x80 and flags & 7:
        raise RuntimeError('ZBIC-compressed NSO: pass the decompressed NSO instead')
    segs = [struct.unpack_from('<4I', d, 0x10 + 0x10 * i) for i in range(3)]
    csz = struct.unpack_from('<3I', d, 0x60)
    img = bytearray(max(m + s for _, m, s, _ in segs))
    for i, (fo, mo, sz, _) in enumerate(segs):
        if flags & (1 << i):
            import lz4.block
            seg = lz4.block.decompress(bytes(d[fo:fo + csz[i]]), uncompressed_size=sz)
        else:
            seg = bytes(d[fo:fo + sz])
        img[mo:mo + len(seg)] = seg
    return bytes(img)


def scan_main(path, out):
    global OUT
    OUT = out
    low_priority()
    d = open(path, 'rb').read()
    if d[:4] == b'NSO0':
        try:
            d = nso_image(d)
        except RuntimeError as e:
            print(f'warning: {path}: {e}; its shaders are left out', file=sys.stderr, flush=True)
            return [], []
    name = os.path.basename(path)
    rows = slim([(name, 0, 0, 0, 'main', go, si, stage_of(ctrl), ctrl, code, gb)
                 for go, gb, secs in glslc_outputs(d) for si, ctrl, code in secs])
    brows, progs = bnsh_rows(name, d)
    return rows + brows, progs


def scan_exefs_nso(path, out):
    """CUDA kernels of one ExeFS NSO -> (rows, kernel rows)."""
    global OUT
    OUT = out
    low_priority()
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        return [], []
    try:
        img = nso_image(d)
    except RuntimeError as e:
        print(f'warning: {path}: {e}; its CUDA kernels are left out', file=sys.stderr, flush=True)
        return [], []
    name = os.path.basename(path)
    occs, kernels = [], []
    for fo, eo, sm, elf in fatbin_cubins(img):
        if sm // 10 != 8:
            continue
        for ki, (k, text, desc, info) in enumerate(cubin_kernels(elf, sm)):
            occs.append((name, fo, 0, 0, 'CUBIN', eo, ki, 'cs', desc, text, None))
            kernels.append((h16(text), k, name, fo, eo, sm, info))
    return slim(occs), kernels


def main():
    global ZSTD_TOOL
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--main')
    ap.add_argument('--exefs', help='ExeFS folder: its sdk/subsdk NSOs are searched for CUDA kernels')
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--zstd', help='zstd or 7-Zip executable for *.zst files (when Python has no zstd module)')
    ap.add_argument('--programs-bin', help="also write the runtime's program list for the shader preparation to this file")
    ap.add_argument('--progress', action='store_true', help='print "scan: <done> <total>" lines while scanning')
    a = ap.parse_args()
    ZSTD_TOOL = find_zstd_tool(a.zstd)
    paths = []
    ktgl = os.path.join(a.romfs, 'motor_asset_re')
    for r, _, fs in os.walk(a.romfs):
        for f in fs:
            low = f.lower()
            low_nz = low[:-4] if low.endswith('.zst') else low
            p = os.path.join(r, f)
            if (low.endswith(('.fdata', '.file')) and p.startswith(ktgl)) or low_nz.endswith(BNSH_FILES):
                paths.append(p)
    paths.sort(key=lambda p: os.path.relpath(p, a.romfs).replace(os.sep, '/'))
    for sub in ('code', 'ctrl', 'glslc'):
        os.makedirs(os.path.join(a.out, sub), exist_ok=True)
    exefs = []
    if a.exefs:
        exefs = sorted(os.path.join(a.exefs, f) for f in os.listdir(a.exefs)
                       if f != 'main' and not f.endswith('.npdm') and os.path.isfile(os.path.join(a.exefs, f)))
    seen = {}
    ctrl_variants = collections.defaultdict(set)
    occ = collections.Counter()
    rows, progs, kernels = [], [], []
    with ProcessPoolExecutor(a.jobs, initializer=_init_worker, initargs=(ZSTD_TOOL,)) as ex:
        futs = [ex.submit(scan_file, p, a.out, os.path.relpath(p, a.romfs).replace(os.sep, '/')) for p in paths]
        if a.main:
            futs.append(ex.submit(scan_main, a.main, a.out))
        kfuts = [ex.submit(scan_exefs_nso, p, a.out) for p in exefs]
        if a.progress:   # "scan: <done> <total>" per finished file (the launcher's setup reads these)
            done = [0]
            total = len(futs) + len(kfuts)
            def tick(_):
                done[0] += 1
                print(f'scan: {done[0]} {total}', flush=True)
            for f in futs + kfuts:
                f.add_done_callback(tick)
        results = [f.result() for f in futs]
        kresults = [f.result() for f in kfuts]
    for res, pr in results + [(r, []) for r, _ in kresults]:
        progs += pr
        for name, eo, fk, tk, tag, go, si, st, cn, kn, h, ch, gh in res:
            occ[h] += 1
            ctrl_variants[h].add(ch)
            if h not in seen:
                seen[h] = (st, kn, cn, name, eo, go, ch, gh)
            rows.append(f'{name}\t{eo:#x}\t{fk:08x}\t{tk:08x}\t{tag}\t{go:#x}\t{si}\t{st}\t{cn:#x}\t{kn:#x}\t{h}\t{ch}\t{gh}')
    for _, kr in kresults:
        kernels += kr
    with open(os.path.join(a.out, 'manifest.tsv'), 'w', newline='\n') as f:
        f.write('source\tentry_off\tfile_ktid\ttype_ktid\tcontainer\tglslc_off\tsection\tstage\tctrl_size\tcode_size'
                '\tcode_sha1_16\tctrl_sha1_16\tglslc_sha1_16\n')
        f.write('\n'.join(rows) + '\n')
    with open(os.path.join(a.out, 'unique.tsv'), 'w', newline='\n') as f:
        f.write('code_sha1_16\tstage\tcode_size\tctrl_size\toccurrences\tctrl_variants\tfirst_source\tentry_off\tglslc_off'
                '\tfirst_ctrl\tfirst_glslc\n')
        for h in sorted(seen):
            st, kn, cn, name, eo, go, ch, gh = seen[h]
            f.write(f'{h}\t{st}\t{kn:#x}\t{cn:#x}\t{occ[h]}\t{len(ctrl_variants[h])}\t{name}\t{eo:#x}\t{go:#x}\t{ch}\t{gh}\n')
    with open(os.path.join(a.out, 'programs.tsv'), 'w', newline='\n') as f:
        f.write('source\tbnsh_off\tvariation\tstages\n')
        for name, base, v, st in progs:
            f.write(f'{name}\t{base:#x}\t{v}\t{st}\n')
    if a.programs_bin:
        pairs = set()
        for name, base, v, st in progs:
            d = dict(x.split('=', 1) for x in st.split())
            if 'vs' in d and 'ps' in d:
                pairs.add((int(d['vs'], 16), int(d['ps'], 16), int(d['gs'], 16) if 'gs' in d else 0))
        pairs = sorted(pairs)
        os.makedirs(os.path.dirname(os.path.abspath(a.programs_bin)), exist_ok=True)
        with open(a.programs_bin, 'wb') as f:
            f.write(b'MPRPRG01' + struct.pack('<II', len(pairs), 0))
            for vs, ps, gs in pairs:
                f.write(struct.pack('<QQQ', vs, ps, gs))
    with open(os.path.join(a.out, 'cuda_kernels.tsv'), 'w', newline='\n') as f:
        f.write('code_sha1_16\tkernel\tsource\tfatbin_off\telf_off\tsm\tparam_off\tparam_size\tshared\tmax_threads\treqntid'
                '\tregs\tbank4\n')
        for h, k, name, fo, eo, sm, i in kernels:
            f.write(f'{h}\t{k}\t{name}\t{fo:#x}\t{eo:#x}\t{sm}\t{i["param_off"]:#x}\t{i["param_size"]:#x}\t{i["shared"]:#x}'
                    f'\t{"x".join(map(str, i["max_threads"]))}\t{"x".join(map(str, i["reqntid"]))}\t{i["regs"]}\t{i["bank4"]:#x}\n')
    by_all = collections.Counter(r.split('\t')[7] for r in rows)
    by_uni = collections.Counter(v[0] for v in seen.values())
    by_kind = collections.Counter(r.split('\t')[4] for r in rows)
    s = [f'occurrences {len(rows)}  unique code blocks {len(seen)}  programs {len(progs)}  CUDA kernels {len(kernels)}']
    for st in sorted(set(by_all) | set(by_uni)):
        s.append(f'  {st}: {by_all[st]} occurrences, {by_uni[st]} unique')
    s.append('containers: ' + ', '.join(f'{k} {v}' for k, v in sorted(by_kind.items())))
    s.append('ctrl sizes (unique code): ' + ', '.join(f'{k:#x}:{v}' for k, v in
                                                     sorted(collections.Counter(v[2] for v in seen.values()).items())))
    open(os.path.join(a.out, 'summary.txt'), 'w', newline='\n').write('\n'.join(s) + '\n')
    print('\n'.join(s))


def _init_worker(tool):
    global ZSTD_TOOL
    ZSTD_TOOL = tool


if __name__ == '__main__':
    main()
