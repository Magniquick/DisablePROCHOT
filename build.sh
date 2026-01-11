#!/bin/bash

gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
    -DHAVE_USE_MS_ABI -Dx86_64 \
    -fPIC -fshort-wchar -ffreestanding -fno-stack-protector -maccumulate-outgoing-args \
    -Wall -Werror \
    -m64 -mno-red-zone \
	-O3 \
    -c -o DisablePROCHOT.o DisablePROCHOT.c

ld -T /usr/lib/elf_x86_64_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
    /usr/lib/crt0-efi-x86_64.o \
    -o DisablePROCHOT.so DisablePROCHOT.o \
    $(gcc -print-libgcc-file-name) /usr/lib/libgnuefi.a /usr/lib/libefi.a

objcopy -j .text -j .sdata -j .rodata -j .data -j .dynamic -j .dynsym -j .rel \
        -j .rela -j .reloc -S --target=efi-app-x86_64 \
        --stack 0x20000,0x20000 \
        DisablePROCHOT.so DisablePROCHOT.efi

rm DisablePROCHOT.o DisablePROCHOT.so

echo "Built DisablePROCHOT.efi"
ls -l DisablePROCHOT.efi
md5sum DisablePROCHOT.efi

gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
    -DHAVE_USE_MS_ABI -Dx86_64 \
    -fPIC -fshort-wchar -ffreestanding -fno-stack-protector -maccumulate-outgoing-args \
    -Wall -Werror \
    -m64 -mno-red-zone \
	-O3 \
    -c -o ChainSuccess.o ChainSuccess.c

ld -T /usr/lib/elf_x86_64_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
    /usr/lib/crt0-efi-x86_64.o \
    -o ChainSuccess.so ChainSuccess.o \
    $(gcc -print-libgcc-file-name) /usr/lib/libgnuefi.a

objcopy -j .text -j .sdata -j .rodata -j .data -j .dynamic -j .dynsym -j .rel \
        -j .rela -j .reloc -S --target=efi-app-x86_64 \
        --stack 0x20000,0x20000 \
        ChainSuccess.so ChainSuccess.efi

echo "Built ChainSuccess.efi (for tests)"
ls -l ChainSuccess.efi
md5sum ChainSuccess.efi

rm ChainSuccess.o ChainSuccess.so
mv ChainSuccess.efi ./test/ChainSuccess.efi

# Build SetBootOrder.efi (test harness)
gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
    -DHAVE_USE_MS_ABI -Dx86_64 \
    -fPIC -fshort-wchar -ffreestanding -fno-stack-protector -maccumulate-outgoing-args \
    -Wall -Werror \
    -m64 -mno-red-zone \
    -O3 \
    -c -o SetBootOrder.o test/SetBootOrder.c

ld -T /usr/lib/elf_x86_64_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
    /usr/lib/crt0-efi-x86_64.o \
    -o SetBootOrder.so SetBootOrder.o \
    $(gcc -print-libgcc-file-name) /usr/lib/libgnuefi.a /usr/lib/libefi.a

objcopy -j .text -j .sdata -j .rodata -j .data -j .dynamic -j .dynsym -j .rel \
        -j .rela -j .reloc -S --target=efi-app-x86_64 \
        --stack 0x20000,0x20000 \
        SetBootOrder.so SetBootOrder.efi

echo "Built SetBootOrder.efi (test harness)"
ls -l SetBootOrder.efi
md5sum SetBootOrder.efi

rm SetBootOrder.o SetBootOrder.so
mv SetBootOrder.efi ./test/SetBootOrder.efi
