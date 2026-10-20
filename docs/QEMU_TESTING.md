# QEMU Emulation & Testing Guide for ios-sweet

This document explains how to test the patched Apple XNU kernel for the Redmi Note 10 Pro (`sweet`, SM7150) in QEMU virtual machine without touching physical hardware or risking device user data.

---

## 1. Overview

`tools/qemu-sweet.py` emulates the bare-metal boot shim loader on your host system:
- It configures `qemu-system-aarch64` on the `virt` board with `gic-version=3` and CPU `cortex-a76` (matching the Kryo 470 Gold cores' `ID_AA64MMFR0` architecture).
- It covers the physical memory layout (framebuffer at `0x9c000000`, kernel base at `0xb0000000`).
- It extracts the visual 1080×2400 framebuffer into `fb.png`, captures registers into `regs.txt`, and logs exception interrupts into `qemu.log`.
- When provided with an unstripped kernel Mach-O, it automatically extracts `_iso_font` and decodes the framebuffer console text into `fb.txt` and `result.json`.

---

## 2. Prerequisites & Setup

Verify your environment using the built-in check:

```sh
python3 tools/qemu-sweet.py --check-env
```

If dependencies are missing, install them:

- **Arch Linux:**
  ```sh
  sudo pacman -S qemu-system-aarch64 llvm
  ```
- **Ubuntu / Debian:**
  ```sh
  sudo apt update && sudo apt install qemu-system-arm llvm
  ```
- **Fedora:**
  ```sh
  sudo dnf install qemu-system-aarch64 llvm
  ```
- **macOS:**
  ```sh
  brew install qemu llvm
  ```

---

## 3. Building a Test Payload for QEMU

1. **Generate the QEMU-compatible Apple Device Tree (ADT):**
   ```sh
   python3 tools/mkadt.py /tmp/qemu.adt --qemu
   ```

2. **Pack the Kernel and ADT into a SWEETXNU Payload:**
   ```sh
   python3 tools/mkpayload.py xnu/out/kernel.development.sweet /tmp/qemu.adt /tmp/payload-qemu.bin \
       --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam rd=md0'
   ```

---

## 4. Running QEMU

### Basic Run (20 Seconds Smoke Test)
```sh
python3 tools/qemu-sweet.py /tmp/payload-qemu.bin /tmp/qemu_run --secs 20
```

### Live Console Streaming
Stream serial kernel printf output live to your terminal:
```sh
python3 tools/qemu-sweet.py /tmp/payload-qemu.bin /tmp/qemu_run --live-serial
```

### Automated CI / Regression Check (Fail on Panic)
Exit with status code `2` if a panic occurred or `3` if blank/undecodable:
```sh
python3 tools/qemu-sweet.py /tmp/payload-qemu.bin /tmp/qemu_run \
    --kernel xnu/out/kernel.development.sweet \
    --fail-on-panic
```

### Interactive GDB Debugging
Start the machine halted with a GDB stub on port 1234:
```sh
python3 tools/qemu-sweet.py /tmp/payload-qemu.bin /tmp/qemu_run --gdb
```
In another terminal:
```sh
gdb -ex "set architecture aarch64" -ex "target remote localhost:1234"
```

---

## 5. Board probe without a phone

`tools/qemu-probe.py` boots the shim's own board probe on QEMU `virt`, so the
probe can be validated before it runs on hardware:

```sh
python3 tools/qemu-probe.py /tmp/qp --secs 12                    # cortex-a53
python3 tools/qemu-probe.py /tmp/qp76 --secs 12 --cpu cortex-a76  # 16K-capable
```

It builds the shim with `-DBOARD_QEMU`, dumps QEMU's own device tree, places it
where the shim expects the pointer in `x0`, and decodes the panel text with the
shim's font into `fb.txt` (plus `fb.png`).

QEMU `virt` is GICv2 here, like the ugglite target, so this exercises the same
interrupt-controller path. Two useful checks:

| CPU | Expected verdict |
| --- | --- |
| `cortex-a53` | `ID: 16K NOT SUPPORTED`, `AT16K` skipped, `4K CPU: XNU needs mixed page sizes` |
| `cortex-a76` | `AT16K ... fail=0`, `16K granule usable` |

`AT4K` should always report `fail=0`; its `PAR_EL1` decodes to the physical
address of `probe_main`, which validates the page tables and the `AT`
mechanism rather than just the CPU's ID register.

## 6. Output Artifacts

Inside the specified output directory:
- `fb.png`: The exact 1080×2400 panel framebuffer output.
- `fb.txt`: Decoded ASCII console text from the framebuffer.
- `regs.txt`: Register state dumped via QMP at the end of the run.
- `qemu.log`: Guest exceptions (interrupts, data aborts, translation faults).
- `result.json`: Automated status summary (`no_panic_observed`, `panic`, `blank_screen`, or `incomplete_decode`).
