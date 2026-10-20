#!/usr/bin/env python3
"""Boot the shim's board probe on QEMU virt, without a phone.

  tools/qemu-probe.py [OUTDIR] [--secs N] [--cpu MODEL] [--fb-phys ADDR]

Host rehearsal for `fastboot boot shim/out/shim-boot-ugglite.img`. It builds
the shim with -DBOARD_QEMU, boots it on QEMU's virt machine (GICv2, like
ugglite), and decodes the text the probe drew into the framebuffer.

What this proves before hardware: the FDT walker against a live flattened
tree, the GICv2 register reads, and the AT-based page-size test. QEMU's
cortex-a53 reports the same ID_AA64MMFR0 as the real MSM8917 (TGran16 = 0),
so the 4K-only verdict should match the phone's.

Outputs in OUTDIR: fb.png, fb.txt, result.json, qemu.log.
"""
import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FB_W, FB_H = 720, 1280
FB_BPP = 4
FB_STRIDE = FB_W * FB_BPP
LOAD_PA = 0x40200000
STUB_PA = 0x40100000
DTB_PA = 0x48000000

# The shim's console: 8x16 glyphs at FB_SCALE=2 (16x32 px), margin 24 px,
# CH_W = 16 px per column, LABEL_COLS = 8.
SCALE = 2
CH_W = 8 * SCALE
CH_H = 16 * SCALE
MARGIN = 24


def make_stub(entry, dtb):
    """Two instructions: x0 = dtb address, then branch to the entry point.

    QEMU's loader device can start a CPU but cannot set x0, and the shim
    expects the DTB pointer there, exactly as ABL passes it.
    """
    hi = (dtb >> 16) & 0xffff
    movz_x0 = 0xd2a00000 | (hi << 5) | 0          # movz x0, #hi, lsl #16
    assert (dtb & 0xffff) == 0, 'dtb address must be 64K aligned'
    off = (entry - (STUB_PA + 4)) // 4
    assert -0x2000000 <= off < 0x2000000, 'branch out of range'
    b = 0x14000000 | (off & 0x3ffffff)
    return struct.pack('<II', movz_x0, b)


def build_shim(fb_phys):
    r = subprocess.run([os.path.join(REPO, 'shim', 'build.sh'), '', 'qemu'],
                       cwd=REPO, capture_output=True, text=True,
                       env={**os.environ, 'EXTRA_CFLAGS': f'-DFB_PHYS={fb_phys:#x}'})
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(1)
    return os.path.join(REPO, 'shim', 'out', 'shim-qemu.img')


def write_png(path, w, h, raw):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    # raw is BGRA; PNG wants RGBA.
    px = bytearray(len(raw))
    px[0::4] = raw[2::4]
    px[1::4] = raw[1::4]
    px[2::4] = raw[0::4]
    px[3::4] = b'\xff' * (len(raw) // 4)
    rows = b''.join(b'\x00' + bytes(px[y * FB_STRIDE:(y + 1) * FB_STRIDE])
                    for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(rows, 6))
                + chunk(b'IEND', b''))


def load_font():
    """Parse shim/font.h into a glyph table, or None if it cannot be parsed."""
    path = os.path.join(REPO, 'shim', 'font.h')
    try:
        text = open(path).read()
    except OSError:
        return None
    glyphs = {}
    import re
    for m in re.finditer(r'\{\s*((?:0x[0-9a-fA-F]{2}\s*,\s*){15}0x[0-9a-fA-F]{2})\s*\}', text):
        rows = bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))
        glyphs[rows] = chr(0x20 + len(glyphs))
    return glyphs or None


def decode(raw, glyphs):
    """Match each character cell against the shim's 8x16 font."""
    lines = []
    max_cols = (FB_W - 2 * MARGIN) // CH_W
    max_rows = (FB_H - 2 * MARGIN) // CH_H
    for r in range(max_rows):
        chars = []
        for c in range(max_cols):
            g = bytearray(16)
            x0 = MARGIN + c * CH_W
            y0 = MARGIN + r * CH_H
            for y in range(16):
                for x in range(8):
                    off = (y0 + y * SCALE) * FB_STRIDE + (x0 + x * SCALE) * 4
                    if raw[off:off + 4] == b'\xff\xff\xff\xff':
                        g[y] |= 0x80 >> x
            chars.append(glyphs.get(bytes(g), ' '))
        lines.append(''.join(chars).rstrip())
    return '\n'.join(lines).strip('\n') + '\n'


def qmp(sock, *cmds, pmemsave=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    f = s.makefile('rw')
    f.readline()

    def call(execute, **args):
        f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                return r

    call('qmp_capabilities')
    out = []
    for c in cmds:
        out.append(call('human-monitor-command', **{'command-line': c}).get('return', ''))
    if pmemsave:
        call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
    s.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir', nargs='?', default='qemu-probe-out')
    ap.add_argument('--secs', type=float, default=20)
    ap.add_argument('--cpu', default='cortex-a53')
    ap.add_argument('--fb-phys', default='0x60000000')
    a = ap.parse_args()

    if not shutil.which('qemu-system-aarch64'):
        sys.exit('qemu-system-aarch64 not found (pacman -S qemu-system-aarch64)')

    fb_phys = int(a.fb_phys, 0)
    image = build_shim(fb_phys)
    os.makedirs(a.outdir, exist_ok=True)
    mon = os.path.join(a.outdir, 'mon.sock')
    if os.path.exists(mon):
        os.unlink(mon)

    ram_gib = 2
    if fb_phys + FB_STRIDE * FB_H > 0x40000000 + (ram_gib << 30):
        ram_gib = -(-(fb_phys + FB_STRIDE * FB_H - 0x40000000) >> 30)

    # QEMU's own device tree for this machine, handed to the shim like ABL does.
    dtb_path = os.path.join(a.outdir, 'virt.dtb')
    subprocess.run(['qemu-system-aarch64', '-M', 'virt,gic-version=2',
                    '-cpu', a.cpu, '-smp', '1', '-m', f'{ram_gib}G',
                    '-machine', f'dumpdtb={dtb_path}'],
                   check=True, capture_output=True)
    stub_path = os.path.join(a.outdir, 'stub.bin')
    open(stub_path, 'wb').write(make_stub(LOAD_PA, DTB_PA))

    cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=2', '-cpu', a.cpu,
           '-smp', '1', '-m', f'{ram_gib}G', '-nographic', '-no-reboot',
           '-qmp', f'unix:{mon},server,nowait',
           '-d', 'guest_errors', '-D', os.path.join(a.outdir, 'qemu.log'),
           '-device', f'loader,file={image},addr={LOAD_PA:#x},force-raw=on',
           '-device', f'loader,file={dtb_path},addr={DTB_PA:#x},force-raw=on',
           '-device', f'loader,file={stub_path},addr={STUB_PA:#x},force-raw=on',
           '-device', f'loader,addr={STUB_PA:#x},cpu-num=0']
    print('[*] ' + ' '.join(cmd), file=sys.stderr)

    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result = {'status': 'unknown'}
    try:
        time.sleep(a.secs)
        if p.poll() is not None:
            result = {'status': 'qemu_exited', 'returncode': p.returncode}
        else:
            raw_path = os.path.join(a.outdir, 'fb.raw')
            qmp(mon, 'stop', pmemsave=(fb_phys, FB_STRIDE * FB_H, raw_path))
            raw = open(raw_path, 'rb').read()
            os.unlink(raw_path)
            write_png(os.path.join(a.outdir, 'fb.png'), FB_W, FB_H, raw)
            glyphs = load_font()
            if glyphs:
                text = decode(raw, glyphs)
                open(os.path.join(a.outdir, 'fb.txt'), 'w').write(text)
                result = {'status': 'decoded', 'text': text}
            else:
                result = {'status': 'no_font'}
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()

    open(os.path.join(a.outdir, 'result.json'), 'w').write(
        json.dumps(result, indent=2) + '\n')
    if 'text' in result:
        print(result['text'])
    else:
        print(json.dumps(result), file=sys.stderr)
    print(f"{a.outdir}/fb.png  {a.outdir}/fb.txt", file=sys.stderr)


if __name__ == '__main__':
    main()
