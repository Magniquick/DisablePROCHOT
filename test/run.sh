#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_IMG="${ROOT_DIR}/test/esp.img"
TMP_DIR="${ROOT_DIR}/test/tmp"
VARS_COPY="${TMP_DIR}/OVMF_VARS.fd"
LOG_FILE="${TMP_DIR}/qemu.log"

if ! command -v "qemu-system-x86_64" >/dev/null 2>&1; then
	echo "Missing required tool: qemu-system-x86_64" >&2
	exit 1
fi

# Build binaries
"${ROOT_DIR}/build.sh"

# Build ESP image
"${ROOT_DIR}/test/make-esp.sh"

mkdir -p "${TMP_DIR}"
rm -f "${LOG_FILE}"
OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"
OVMF_VARS="/usr/share/edk2/x64/OVMF_VARS.4m.fd"

if [ ! -f "${OVMF_CODE}" ]; then
	echo "Could not find OVMF_CODE.fd." >&2
	exit 1
fi
if [ ! -f "${OVMF_VARS}" ]; then
	echo "Could not find OVMF_VARS.fd." >&2
	exit 1
fi

cp -f "${OVMF_VARS}" "${VARS_COPY}"

env TMPDIR="${TMP_DIR}" timeout 30s qemu-system-x86_64 \
	-machine q35,accel=kvm:tcg \
	-m 256 \
	-drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
	-drive if=pflash,format=raw,file="${VARS_COPY}" \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=esp,format=raw,file="${ESP_IMG}" \
	-device usb-storage,bus=xhci.0,drive=esp,removable=on,bootindex=1 \
	-nographic \
	-no-reboot 2>&1 | tee "${LOG_FILE}"

grep -q "Created Boot0004 -> EFI USB Device" "${LOG_FILE}"
grep -q "Created Boot0005 -> Inactive" "${LOG_FILE}"
grep -q "Created Boot0006 -> Missing" "${LOG_FILE}"
grep -q "Chainload successful" "${LOG_FILE}"
