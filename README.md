# DisablePROCHOT

[Build workflow](https://github.com/Magniquick/DisablePROCHOT/actions/workflows/build.yml) | [Releases](https://github.com/Magniquick/DisablePROCHOT/releases)

`DisablePROCHOT` is a small x86_64 UEFI application that clears the BD PROCHOT control bit at boot, then chains to the next UEFI boot entry.

BD PROCHOT (Bi-Directional PROCHOT) can force very low CPU clocks (for example ~400 MHz) when platform firmware or sensors assert thermal throttling. This project is useful when BD PROCHOT is being triggered incorrectly.

## Important Safety Warning

Disabling BD PROCHOT removes a hardware thermal-throttling signal path. Use this only if you understand the risks and have verified your cooling and sensor health.

You are responsible for any hardware damage, instability, or data loss.

## Why This Exists

Tools like ThrottleStop run after the OS starts. If BD PROCHOT is stuck, your entire boot sequence can remain slow.

This EFI app runs before OS boot, so the machine is not stuck at minimum clocks during startup.

## What It Does

On real hardware:
- Writes `0` to MSR `0x1FC` (`IA32_POWER_CTL`) to clear BD PROCHOT enable.
- Attempts to chainload the next boot option from `BootOrder`.

In virtualized test environments:
- Detects hypervisor presence and skips the MSR write.
- Still exercises chainloading logic.

## Limitations

- ACPI S3 suspend/resume can re-enable BD PROCHOT.
- If that happens, use an OS-level tool after resume.
  - Linux: [Post-suspend workaround gist](https://gist.github.com/Magniquick/0862e7dc354f060caf52ca96f36e3f4b)
  - Windows: ThrottleStop
  - macOS: [SimpleMSR](https://github.com/arter97/SimpleMSR)

## Build

Requirements (typical GNU-EFI setup):
- `gcc`, `ld`, `objcopy`
- GNU-EFI headers/libs (`/usr/include/efi`, `/usr/lib/libgnuefi.a`, `/usr/lib/libefi.a`)

Build everything:

```bash
./build.sh
```

This generates:
- `DisablePROCHOT.efi`
- `test/ChainSuccess.efi`
- `test/SetBootOrder.efi`

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
