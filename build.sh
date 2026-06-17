#!/bin/bash
set -euo pipefail

# -----------------------------------------------------------------------------
# Everything is built natively to PE-COFF with clang + lld -- no GNU-EFI
# runtime, no objcopy ELF->PE conversion. UEFI binaries are just PE32+ with the
# MS x64 ABI, which clang's x86_64-unknown-windows target emits directly.
# GNU-EFI headers are used for TYPES only; all runtime helpers live in the .c.
#
#   -mno-sse -mgeneral-regs-only : no SSE/AVX/x87/MMX may be emitted (firmware
#                                  has no guaranteed FP/vector state)
#   -mno-red-zone                : mandatory in firmware context
#   -Os -flto + /opt:ref,icf     : dead-code elimination -> a few KB
# Default page alignment keeps W^X and broad firmware compatibility.
# -----------------------------------------------------------------------------
CLANG=(
	clang --target=x86_64-unknown-windows
	# GNU-EFI headers as -isystem so their warnings stay out of our build
	# (transparent to clang) while our own code is fully checked.
	-isystem /usr/include/efi -isystem /usr/include/efi/x86_64
	-DHAVE_USE_MS_ABI -Dx86_64
	-std=c17 -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector
	-mno-sse -mgeneral-regs-only -fno-builtin
	-fno-unwind-tables -fno-asynchronous-unwind-tables
	-Os -ffunction-sections -fdata-sections -flto
	-Wall -Wextra -Wpedantic -Wundef -Wshadow -Wpointer-arith -Wdouble-promotion -Wconversion
	-Werror -Wno-error=pedantic  # pedantic informs but never fails the build
	-nostdlib -fuse-ld=lld
	-Wl,-subsystem:efi_application -Wl,-entry:efi_main -Wl,/opt:ref -Wl,/opt:icf
)

build() { # <out.efi> <src.c>
	"${CLANG[@]}" -o "$1" "$2"
	echo "Built $1 ($(stat -c%s "$1") bytes)"
}

build DisablePROCHOT.efi      DisablePROCHOT.c
md5sum DisablePROCHOT.efi

# Test-harness helpers (only needed for ./test/run.sh).
build test/ChainSuccess.efi   test/ChainSuccess.c
build test/WrongTarget.efi    test/WrongTarget.c
build test/SetBootOrder.efi   test/SetBootOrder.c
