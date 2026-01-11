# QEMU EFI test harness

This harness boots DisablePROCHOT.efi in OVMF and chainloads ChainSuccess.efi
from a real FAT ESP image. Console output should include:

```
Disabling BD PROCHOT
BD PROCHOT disabled
Fallback chain to \EFI\BOOT\CHAIN.EFI
Chainload successful
```

## Requirements
- `qemu-system-x86_64`
- OVMF firmware (Arch package: `edk2-ovmf`)
- `mtools` (provides `mcopy`, `mmd`) and `mkfs.vfat`

## Usage
```
./test/run.sh          # builds EFIs, rebuilds ESP, runs qemu/OVMF
./test/make-esp.sh     # just rebuild the FAT ESP image
```
