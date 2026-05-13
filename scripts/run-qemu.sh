#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

build_dir=${TORI_BUILD_DIR:-"$repo_root/build"}
cc=${CC:-clang}
cxx=${CXX:-clang++}
ovmf_code=${TORI_OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}
ovmf_vars_template=${TORI_OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}
ovmf_vars="$build_dir/OVMF_VARS.4m.fd"
iso="$build_dir/tori.iso"

if [ ! -f "$ovmf_code" ]; then
    echo "missing OVMF code image: $ovmf_code" >&2
    echo "set TORI_OVMF_CODE to override" >&2
    exit 1
fi

if [ ! -f "$ovmf_vars_template" ]; then
    echo "missing OVMF vars template: $ovmf_vars_template" >&2
    echo "set TORI_OVMF_VARS_TEMPLATE to override" >&2
    exit 1
fi

cmake -S "$repo_root" -B "$build_dir" \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DTORI_OVMF_CODE="$ovmf_code" \
    -DTORI_OVMF_VARS_TEMPLATE="$ovmf_vars_template"

cmake --build "$build_dir" --target iso
cmake -E copy_if_different "$ovmf_vars_template" "$ovmf_vars"

exec qemu-system-x86_64 \
    -machine q35 \
    -m "${TORI_QEMU_MEMORY:-2G}" \
    -smp "${TORI_QEMU_CPUS:-4}" \
    -serial stdio \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$ovmf_vars" \
    -cdrom "$iso" \
    "$@"
