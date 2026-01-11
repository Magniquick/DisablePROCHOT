#!/bin/bash

build_efi() {
	local name="$1"
	local src="$2"
	local libs="$3"
	local dest="$4"

	# drop -O3 to debug
	gcc -I/usr/include/efi -I/usr/include/efi/x86_64 \
		-DHAVE_USE_MS_ABI -Dx86_64 \
		-fPIC -fshort-wchar -ffreestanding -fno-stack-protector -maccumulate-outgoing-args \
		-Wall -Werror \
		-m64 -mno-red-zone -O3 \
		-c -o "${name}.o" "$src"

	ld -T /usr/lib/elf_x86_64_efi.lds -Bsymbolic -shared -nostdlib -znocombreloc \
		/usr/lib/crt0-efi-x86_64.o \
		-o "${name}.so" "${name}.o" \
		$(gcc -print-libgcc-file-name) $libs

	objcopy -j .text -j .sdata -j .rodata -j .data -j .dynamic -j .dynsym -j .rel \
		-j .rela -j .reloc -S --target=efi-app-x86_64 \
		--stack 0x20000,0x20000 \
		"${name}.so" "${name}.efi"

	rm "${name}.o" "${name}.so"

	if [ -n "$dest" ]; then
		mv "${name}.efi" "$dest"
		echo "Built ${dest}"
		ls -l "$dest"
		md5sum "$dest"
	else
		echo "Built ${name}.efi"
		ls -l "${name}.efi"
		md5sum "${name}.efi"
	fi
}

build_efi "DisablePROCHOT" "DisablePROCHOT.c" "/usr/lib/libgnuefi.a /usr/lib/libefi.a"
build_efi "ChainSuccess" "test/ChainSuccess.c" "/usr/lib/libgnuefi.a" "./test/ChainSuccess.efi"
build_efi "SetBootOrder" "test/SetBootOrder.c" "/usr/lib/libgnuefi.a /usr/lib/libefi.a" "./test/SetBootOrder.efi"
