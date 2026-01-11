#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_IMG="${ROOT_DIR}/test/esp.img"
VARS_COPY="${ROOT_DIR}/test/OVMF_VARS.fd"
TMP_DIR="${ROOT_DIR}/test/tmp"

if ! command -v "qemu-system-x86_64" >/dev/null 2>&1; then
	echo "Missing required tool: qemu-system-x86_64" >&2
	exit 1
fi

# Build binaries
"${ROOT_DIR}/build.sh"

# Build ESP image
"${ROOT_DIR}/test/make-esp.sh"

mkdir -p "${TMP_DIR}"
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

exec env TMPDIR="${TMP_DIR}" qemu-system-x86_64 \
	-machine q35,accel=kvm:tcg \
	-m 256 \
	-drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
	-drive if=pflash,format=raw,file="${VARS_COPY}" \
	-drive file="${ESP_IMG}",if=ide,format=raw \
	-nographic \
	-no-reboot
