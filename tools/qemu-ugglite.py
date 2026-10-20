#!/usr/bin/env python3
"""Boot an ugglite SWEETXNU payload on QEMU virt and report what it reached.

  tools/qemu-ugglite.py PAYLOAD OUTDIR [--secs N] [--gdb PORT] [--phys-base A]

This is the ugglite profile of tools/qemu-sweet.py: `-M virt,gic-version=2
-cpu cortex-a53` (the A53 model reports the same ID_AA64MMFR0 as the real
MSM8917, TGran16 = 0), the ugglite ADT, and the 4K kernel. It exists because
the 0082-0087 bring-up failed on hardware at `MSR SCTLR_EL1` with no output,
and QEMU reproduces that fault exactly - see NOTES.md, "QEMU reproduction".

What it reports
  qemu.log   exceptions with ESR/ELR/FAR (-d int,guest_errors)
  regs.txt   CPU state at the stop
  fb.txt     the 720x1280 RGB888 splash framebuffer, summarised by colour
  result.json

The interesting signal is the framebuffer, not the absence of exceptions.
arm_init()'s first act is `sweet_early_mark(1, ...)`, which fills the whole
panel with 0x00008000 (dark green). Seeing ~82% dark green means execution
left start.s, enabled the MMU and reached C code; a blank panel means it did
not. A subsequent Data Abort at FAR 0x220 is expected and is the next bug.

Why 2 MiB alignment matters
  start.s maps the kernel with 2 MiB L2 blocks and masks the physical address
  with ARM_TTE_BLOCK_L2_MASK (0xffffffe00000). A phys_base that is not 2 MiB
  aligned is silently rounded down, so the V=P mapping lands below the kernel
  and the first fetch after the MMU enable faults. This runner refuses an
  unaligned base unless --allow-unaligned is passed, because that is exactly
  the bug that cost several CI runs.

gdb recipe (--gdb 1234, QEMU starts halted)
  gdb -q -ex 'set architecture aarch64' -ex 'target remote :1234'
  # kernel link VA 0xfffffe00074280ec (_start_first_cpu); with
  # virt_base 0xfffffe0007000000 and phys_base 0x8d600000 the PA is
  # link_va - 0xfffffe0007000000 + 0x8d600000. Break there, then step.
  # `hb *0x8d9286d4` is the instruction after MSR SCTLR_EL1 for the 0087
  # image at 0x8d500000; recompute for other bases.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time

FB_BASE, FB_W, FB_H, FB_STRIDE, FB_BPP = 0x90000000, 720, 1280, 2160, 3
PAGE = 0x1000
STUB_PA = 0x40200000
BOOT_LINE_LENGTH = 608
L2_MASK = 0x1fffff


def build_image(payload):
    """Return (phys_base, ram_image, boot_args_pa, entry_pa), as shim/loader.c does."""
    magic, ver, nsegs = struct.unpack_from('<8sII', payload, 0)
    assert magic == b'SWEETXNU' and ver == 1, 'not a SWEETXNU v1 payload'
    virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size = \
        struct.unpack_from('<7Q', payload, 0x10)
    cmdline = payload[0x48:0x48 + BOOT_LINE_LENGTH]
    delta = phys_base - virt_base
    adt_pa = kernel_end + delta
    ba_pa = (adt_pa + adt_size + PAGE - 1) & ~(PAGE - 1)
    top = ba_pa + PAGE
    img = bytearray(top - phys_base)
    for i in range(nsegs):
        vmaddr, vmsize, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        o = vmaddr + delta - phys_base
        img[o:o + fsize] = payload[foff:foff + fsize]
    img[adt_pa - phys_base:adt_pa - phys_base + adt_size] = payload[adt_off:adt_off + adt_size]
    ba = struct.pack('<HH4x4Q6QI4xQI', 2, 2, virt_base, phys_base, mem_size, top,
                     FB_BASE, 1, FB_STRIDE, FB_W, FB_H, FB_BPP * 8, 0,
                     adt_pa - delta, adt_size)
    ba += cmdline + b'\0' * 4 + struct.pack('<QQ', 0, mem_size)
    assert len(ba) == 736
    img[ba_pa - phys_base:ba_pa - phys_base + len(ba)] = ba
    return phys_base, img, ba_pa, entry_va + delta


def stub(ba_pa, entry_pa):
    return struct.pack('<4I2Q', 0x58000080, 0x580000a1, 0xd61f0020, 0xd503201f, ba_pa, entry_pa)


# --- patch 0088, applied to a prebuilt image -------------------------------
#
# The runner has no macOS toolchain, so it cannot rebuild a kernel. It can
# still prove patch 0088's two edits are the fix by rewriting the same words
# in a kernel that was built *before* the patch:
#
#   1. TCR_EL1's T0SZ/T1SZ 23 -> 16, i.e. the 48-bit/4-level geometry whose
#      9-bit L0 index matches ARM_4K_TT_L0_INDEX_MASK (see proc_reg.h).
#   2. Every bring-up marker uses w16 instead of w14, so marker 1 stops
#      clobbering x14 (the KVA virtual cursor) before the KVA mapping is built.
#
# Both are located by symbol, not by hardcoded offset, so the same helper works
# for any phys_base and for kernels built after 0085.
MARKER_OLD = struct.pack('<4I', 0x3800154b, 0x53087d6e, 0x3800154e, 0x53107d6e)
MARKER_NEW = struct.pack('<4I', 0x3800154b, 0x53087d70, 0x38001550, 0x53107d70)


def patch_0088(img, virt_base, phys_base, tcr_off):
    """Apply patch 0088's edits to a built kernel image in place.

    tcr_off is the link VA of the sysreg_restore slot that start.s loads
    TCR_EL1 from (SR_RESTORE_TCR_EL1); see `llvm-nm ... | grep sysreg_restore`.
    """
    def pa(link_va):
        return link_va - virt_base + phys_base

    off = pa(tcr_off) - phys_base
    old = struct.unpack_from('<Q', img, off)[0]
    new = (old & ~0x3f & ~(0x3f << 16)) | 16 | (16 << 16)
    struct.pack_into('<Q', img, off, new)

    n = img.count(MARKER_OLD)
    img[:] = img.replace(MARKER_OLD, MARKER_NEW)
    img[:] = img.replace(struct.pack('<I', 0x3800154e), struct.pack('<I', 0x38001550))
    return {'tcr': [hex(old), hex(new)], 'marker_blocks': n}


def qmp(sock_path, *cmds, pmemsave=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
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
        r = call('human-monitor-command', **{'command-line': c})
        out.append(r.get('return', ''))
    if pmemsave:
        call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
    s.close()
    return out


def summarise_fb(raw):
    """Colour histogram plus the row span of the arm_init stage-1 fill."""
    from collections import Counter
    c = Counter(raw[i:i + FB_BPP] for i in range(0, len(raw) - FB_BPP + 1, FB_BPP))
    total = len(raw) // FB_BPP
    green = c.get(b'\x00\x80\x00', 0)
    rows = [y for y in range(FB_H)
            if Counter(raw[y * FB_STRIDE + i:y * FB_STRIDE + i + FB_BPP]
                       for i in range(0, FB_STRIDE - FB_BPP + 1, FB_BPP)
                       ).most_common(1)[0][0] == b'\x00\x80\x00']
    return {
        'colours': [{'rgb': k.hex(), 'pixels': v, 'pct': round(100.0 * v / total, 2)}
                    for k, v in c.most_common(6)],
        'arm_init_green_pct': round(100.0 * green / total, 2),
        'arm_init_green_rows': [rows[0], rows[-1]] if rows else None,
        'reached_arm_init': green > total // 2,
    }


def main():
    ap = argparse.ArgumentParser(description='Boot an ugglite payload on QEMU virt (GICv2, cortex-a53).')
    ap.add_argument('payload')
    ap.add_argument('outdir')
    ap.add_argument('--secs', type=float, default=15.0)
    ap.add_argument('--gdb', type=int, help='start halted with a gdb stub on this TCP port')
    ap.add_argument('--allow-unaligned', action='store_true',
                    help='permit a phys_base that is not 2 MiB aligned (known to fault)')
    ap.add_argument('--patch-0088', metavar='TCR_LINK_VA',
                    help='rewrite the prebuilt kernel as patch 0088 would, e.g. '
                         '0xfffffe0007174a10 (the sysreg_restore TCR_EL1 slot)')
    a = ap.parse_args()

    if not subprocess.run(['sh', '-c', 'command -v qemu-system-aarch64'], capture_output=True).returncode == 0:
        print('[!] qemu-system-aarch64 not found in PATH', file=sys.stderr)
        return 1

    payload = open(a.payload, 'rb').read()
    phys_base, img, ba_pa, entry_pa = build_image(payload)
    if a.patch_0088:
        virt_base, = struct.unpack_from('<Q', payload, 0x10)
        info = patch_0088(img, virt_base, phys_base, int(a.patch_0088, 0))
        print(f'[patch-0088] TCR {info["tcr"][0]} -> {info["tcr"][1]}, '
              f'{info["marker_blocks"]} marker blocks moved to w16', file=sys.stderr)
    if phys_base & L2_MASK and not a.allow_unaligned:
        print(f'[!] phys_base {phys_base:#x} is not 2 MiB aligned: start.s would map '
              f'{phys_base & ~L2_MASK:#x} instead. Use --allow-unaligned to test anyway.',
              file=sys.stderr)
        return 2
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.abspath(a.outdir)
    open(f'{out}/ram.bin', 'wb').write(img)
    open(f'{out}/stub.bin', 'wb').write(stub(ba_pa, entry_pa))
    print(f'phys_base={phys_base:#x} boot_args={ba_pa:#x} entry={entry_pa:#x}', file=sys.stderr)

    mon = f'{out}/mon.sock'
    if os.path.exists(mon):
        os.unlink(mon)
    ram_gib = max(2, -(-(phys_base + len(img) - 0x40000000) // (1 << 30)))
    cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=2', '-cpu', 'cortex-a53',
           '-smp', '1', '-m', f'{ram_gib}G', '-display', 'none', '-serial', 'none',
           '-qmp', f'unix:{mon},server,nowait',
           '-d', 'int,guest_errors', '-D', f'{out}/qemu.log', '-no-reboot',
           '-device', f'loader,file={out}/ram.bin,addr={phys_base:#x},force-raw=on',
           '-device', f'loader,file={out}/stub.bin,addr={STUB_PA:#x},force-raw=on',
           '-device', f'loader,addr={STUB_PA:#x},cpu-num=0']
    if a.gdb:
        cmd += ['-S', '-gdb', f'tcp::{a.gdb}']

    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if a.gdb:
        print(f'gdb stub on :{a.gdb}; QMP socket {mon}', file=sys.stderr)
        p.wait()
        return 0

    time.sleep(a.secs)
    result = {'phys_base': hex(phys_base)}
    try:
        regs, _ = qmp(mon, 'stop', 'info registers',
                      pmemsave=(FB_BASE, FB_STRIDE * FB_H, f'{out}/fb.raw'))
        open(f'{out}/regs.txt', 'w').write(regs)
        raw = open(f'{out}/fb.raw', 'rb').read()
        os.unlink(f'{out}/fb.raw')
        result.update(summarise_fb(raw))
        with open(f'{out}/fb.txt', 'w') as f:
            f.write(f"arm_init green: {result['arm_init_green_pct']}% "
                    f"rows {result['arm_init_green_rows']}\n")
            for c in result['colours']:
                f.write(f"  #{c['rgb']} {c['pixels']:8d} ({c['pct']:5.2f}%)\n")
    except Exception as e:
        result['error'] = str(e)
    p.kill()
    p.wait()

    if os.path.exists(f'{out}/qemu.log'):
        lines = open(f'{out}/qemu.log', errors='replace').read().splitlines()
        exc = [' '.join(x.strip('. ') for x in lines[i:i + 5])
               for i, l in enumerate(lines) if l.startswith('Taking exception')]
        result['exceptions'] = len(exc)
        result['first_exception'] = exc[0] if exc else None
    result['status'] = 'reached_arm_init' if result.get('reached_arm_init') else 'did_not_reach_arm_init'
    open(f'{out}/result.json', 'w').write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result.get('reached_arm_init') else 3


if __name__ == '__main__':
    sys.exit(main())
