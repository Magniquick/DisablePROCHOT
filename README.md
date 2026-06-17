# DisablePROCHOT

[Build workflow](https://github.com/Magniquick/DisablePROCHOT/actions/workflows/build.yml) | [Releases](https://github.com/Magniquick/DisablePROCHOT/releases)

`DisablePROCHOT` is a tiny (~4.5 KB) x86_64 UEFI application that performs a **full thermal-throttle unlock** at boot, then chainloads the next UEFI boot entry.

BD PROCHOT (Bi-Directional PROCHOT) can force very low CPU clocks (for example ~400 MHz) when platform firmware or sensors assert thermal throttling. On some platforms a phantom VR-thermal-alert clamp also pins the CPU and iGPU to base clock even when thermals are fine. This project is useful when either is being triggered incorrectly.

## Important Safety Warning

This removes hardware thermal-throttling signal paths. Use this only if you understand the risks and have verified your cooling and sensor health.

You are responsible for any hardware damage, instability, or data loss.

## Why This Exists

Tools like ThrottleStop run after the OS starts. If the throttle is stuck, your entire boot sequence can remain slow.

This EFI app runs before OS boot, so the machine is not stuck at minimum clocks during startup.

## What It Does (full unlock)

On real hardware it does a read-modify-write of MSR `0x1FC` (`IA32_POWER_CTL`), touching only the two bits it cares about and preserving every other firmware-set bit:

- **bit 0 = 0**: clears BD PROCHOT enable (bi-directional processor-hot throttling).
- **bit 24 = 1**: sets `DISABLE_VR_THERMAL_ALERT`, lifting the phantom VR-thermal clamp that otherwise pins CPU + iGPU to base clock.

Together these are the "full unlock": both the PROCHOT path and the VR-thermal-alert path are released in one shot. The MSR never `#GP`s on real hardware (and QEMU/OVMF silently emulates it), so there is no hypervisor check or fault handler - it just does the write.

It then chainloads the next loadable entry in `BootOrder`. Because firmware `Boot####` entries are stored in short form (`HD(signature)/File`, no hardware prefix) and many firmwares' `LoadImage` won't expand them, the app rebuilds a full device path from its own boot partition before loading - so the chainload works on real machines, not just in QEMU.

## Limitations

- ACPI S3 suspend/resume can re-enable BD PROCHOT.
- If that happens, use an OS-level tool after resume.
  - Linux: [Post-suspend workaround gist](https://gist.github.com/Magniquick/0862e7dc354f060caf52ca96f36e3f4b)
  - Windows: ThrottleStop
  - macOS: [SimpleMSR](https://github.com/arter97/SimpleMSR)

## Build

The application is compiled **natively to PE-COFF with clang + lld**: no GNU-EFI runtime, no `objcopy` ELF-to-PE conversion (UEFI binaries are just PE32+ with the MS x64 ABI, which `clang --target=x86_64-unknown-windows` emits directly). GNU-EFI's headers are used for type definitions only; all runtime helpers live in `DisablePROCHOT.c`.

Requirements:
- `clang` and `lld`
- GNU-EFI headers (`/usr/include/efi`) - headers only

Build:

```bash
./build.sh
```

This generates `DisablePROCHOT.efi` (~4.5 KB). If a full GNU-EFI toolchain is also installed, `build.sh` additionally builds the QEMU test helpers (`test/*.efi`); otherwise it skips them.

Prebuilt binaries are attached to each [release](https://github.com/Magniquick/DisablePROCHOT/releases).

## Installation / Bootloader Integration

`DisablePROCHOT.efi` respects the UEFI `BootOrder` variable and chainloads the next entry.
This means a boot manager such as Clover or rEFInd is optional.

You can configure firmware boot entries directly, for example on Linux:

```bash
efibootmgr --bootorder 0002,0000
```

In this example, `0002` is `DisablePROCHOT` and `0000` is your normal OS loader.

For a step-by-step setup/verification flow, see `CHAINLOAD.md`.

If you prefer Clover, placing `DisablePROCHOT.efi` in `drivers64UEFI` also works.

## Test Harness (QEMU + OVMF)

A reproducible test flow exists under `test/`.

Requirements:
- `qemu-system-x86_64`
- OVMF firmware (for Arch Linux, package `edk2-ovmf`)
- `mtools` (`mcopy`, `mmd`) and `mkfs.vfat`
- the GNU-EFI toolchain (for the test helpers)

Run full test:

```bash
./test/run.sh
```

See `test/README.md` for expected output and details.

## Upstream Attribution

This project is based on upstream work by Park Ju Hyung (arter97):
- Upstream repository: <https://github.com/arter97/DisablePROCHOT>
- Copyright notice is retained in source.

This repository contains downstream maintenance and additional improvements.

## License

See `LICENSE`.
