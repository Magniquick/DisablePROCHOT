#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_IMG="${ROOT_DIR}/test/esp.img"
EFI_DISABLE="${ROOT_DIR}/DisablePROCHOT.efi"
EFI_CHAIN="${ROOT_DIR}/test/ChainSuccess.efi"
EFI_SETBOOT="${ROOT_DIR}/test/SetBootOrder.efi"
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

if [ ! -f "${EFI_DISABLE}" ] || [ ! -f "${EFI_CHAIN}" ] || [ ! -f "${EFI_SETBOOT}" ]; then
	echo "Missing EFI binaries. Run ./build.sh first." >&2
	exit 1
fi

mkdir -p "${ROOT_DIR}/test"
truncate -s "${SIZE_MB}M" "${ESP_IMG}"
mkfs.vfat -F32 "${ESP_IMG}"

MTOOLS_SKIP_CHECK=1 mmd -i "${ESP_IMG}" ::/EFI ::/EFI/BOOT

# SetBootOrder.efi is the default boot entry - it sets up BootOrder and launches DisablePROCHOT
MTOOLS_SKIP_CHECK=1 mcopy -i "${ESP_IMG}" "${EFI_SETBOOT}" ::/EFI/BOOT/BOOTX64.EFI

# DisablePROCHOT.efi and ChainSuccess.efi are chainloaded via BootOrder
MTOOLS_SKIP_CHECK=1 mcopy -i "${ESP_IMG}" "${EFI_DISABLE}" ::/EFI/BOOT/DisablePROCHOT.efi
MTOOLS_SKIP_CHECK=1 mcopy -i "${ESP_IMG}" "${EFI_CHAIN}" ::/EFI/BOOT/ChainSuccess.efi
