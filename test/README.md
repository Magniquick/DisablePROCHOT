# QEMU EFI test harness

Tests the BootOrder chainloading path (not the CHAIN.EFI fallback):

1. **SetBootOrder.efi** (installed as BOOTX64.EFI) runs first:
   - Overwrites Boot0002 → DisablePROCHOT.efi
   - Creates Boot0003 → ChainSuccess.efi
   - Sets BootOrder = {0002, 0003}
   - Firmware already set BootCurrent = 0002 when it booted us
   - Launches DisablePROCHOT.efi

2. **DisablePROCHOT.efi** runs:
   - Detects hypervisor, skips MSR write
   - Reads BootOrder = {0002, 0003}, BootCurrent = 0002
   - Finds BootCurrent at index 0, next entry is Boot0003
   - Chainloads Boot0003 (ChainSuccess.efi)

3. **ChainSuccess.efi** runs:
   - Prints "Chainload successful"
   - Shuts down

## Expected output

```
SetBootOrder: Setting up boot variables
Created Boot0002 -> DisablePROCHOT.efi
Created Boot0003 -> ChainSuccess.efi
Set BootOrder = {0002, 0003}
BootCurrent = 0002 (set by firmware)
Launching DisablePROCHOT.efi...
Hypervisor detected, skipping MSR write
Chainloading next boot entry
Chainload successful
Shutting down
```

## Requirements
- `qemu-system-x86_64`
- OVMF firmware (Arch: `edk2-ovmf`)
- `mtools` (`mcopy`, `mmd`) and `mkfs.vfat`

## Usage
```
./test/run.sh          # builds EFIs, rebuilds ESP, runs qemu/OVMF
./test/make-esp.sh     # just rebuild the FAT ESP image
```
