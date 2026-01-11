#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_IMG="${ROOT_DIR}/test/esp.img"
EFI_BOOT="${ROOT_DIR}/DisablePROCHOT.efi"
EFI_CHAIN="${ROOT_DIR}/test/ChainSuccess.efi"
SIZE_MB=64

need() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Missing required tool: $1" >&2
		exit 1
	fi
}

need mkfs.vfat
need mcopy
need mmd

if [ ! -f "${EFI_BOOT}" ] || [ ! -f "${EFI_CHAIN}" ]; then
	echo "Missing EFI binaries. Run ./build.sh first." >&2
	exit 1
fi

mkdir -p "${ROOT_DIR}/test"
truncate -s "${SIZE_MB}M" "${ESP_IMG}"
mkfs.vfat -F32 "${ESP_IMG}"

MTOOLS_SKIP_CHECK=1 mmd -i "${ESP_IMG}" ::/EFI ::/EFI/BOOT
MTOOLS_SKIP_CHECK=1 mcopy -i "${ESP_IMG}" "${EFI_BOOT}" ::/EFI/BOOT/BOOTX64.EFI
MTOOLS_SKIP_CHECK=1 mcopy -i "${ESP_IMG}" "${EFI_CHAIN}" ::/EFI/BOOT/CHAIN.EFI
