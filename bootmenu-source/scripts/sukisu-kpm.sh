#!/usr/bin/env bash
# Build the SukiSU KernelPatch runtime, then inject it into a raw ARM64 Image.
set -euo pipefail

kernel_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kpm_root="$kernel_root/KPM"

check_abi() {
    python3 - "$kernel_root" <<'PY'
import re
import sys
from pathlib import Path
root = Path(sys.argv[1])
headers = [root / 'KernelSU/kernel/extension/kpm/bridge.h',
           root / 'KPM/kernel/patch/sukisu/supercall.h']
definitions = [dict(re.findall(r'^#define\s+(SUKISU_KPM_\w+)\s+(\S+)',
                              p.read_text(), re.M)) for p in headers]
for name in ('SUKISU_KPM_SUPERCALL_MAGIC', 'SUKISU_KPM_SUPERCALL_MAGIC2',
             'SUKISU_KPM_SUPERCALL_MAGIC3', 'SUKISU_KPM_ABI_VERSION',
             'SUKISU_KPM_PROBE', 'SUKISU_KPM_OUTPUT_MAX'):
    if name not in definitions[0] or definitions[0][name] != definitions[1].get(name):
        raise SystemExit(f'SukiSU/KernelPatch ABI mismatch: {name}')
print('SukiSU/KernelPatch bridge ABI matches')
PY
}

case "${1:-}" in
    build)
        [[ $# -eq 1 ]] || exit 2
        check_abi
        cross=${KPM_CROSS_COMPILE:-aarch64-linux-gnu-}
        compiler=${KPM_CC:-${cross}gcc}
        jobs=${JOBS:-$(nproc)}
        command -v "$compiler" >/dev/null
        command -v "${cross}ld" >/dev/null
        command -v "${cross}objcopy" >/dev/null
        make -C "$kpm_root/tools" -j"$jobs" CC="${HOSTCC:-cc}"
        # This standalone runtime has no libc stack-canary support. These flags
        # apply only to kpimg, never to the Linux kernel's hardening options.
        CFLAGS='-fno-stack-protector -fno-pie -fno-pic -mno-outline-atomics' \
            make -B -C "$kpm_root/kernel" -j"$jobs" \
            TARGET_COMPILE="$cross" CC="$compiler"
        "$kpm_root/tools/kptools" -l -k "$kpm_root/kernel/kpimg"
        ;;
    patch)
        [[ $# -eq 3 ]] || { echo "Usage: $0 patch Image Image.kpm" >&2; exit 2; }
        check_abi
        input=$(realpath -e -- "$2")
        output=$(realpath -m -- "$3")
        [[ "$input" != "$output" && ! -e "$output" ]] || {
            echo 'Output must be a new file, distinct from the input' >&2
            exit 2
        }
        python3 - "$input" <<'PY'
import sys
with open(sys.argv[1], 'rb') as stream:
    header = stream.read(64)
if header[56:60] != b'ARM\x64':
    raise SystemExit('Input must be an uncompressed ARM64 Image, not boot.img or Image.gz')
PY
        [[ -x "$kpm_root/tools/kptools" && -s "$kpm_root/kernel/kpimg" ]] || {
            echo "Run $0 build first" >&2
            exit 2
        }
        temporary=$(mktemp -- "$(dirname -- "$output")/.sukisu-kpm.XXXXXXXX")
        trap 'rm -f -- "$temporary" "${temporary}.info"' EXIT
        # Same compatibility placeholder as the official SukiSU manager.
        "$kpm_root/tools/kptools" -p -s 123 -i "$input" \
            -k "$kpm_root/kernel/kpimg" -o "$temporary"
        [[ -s "$temporary" ]]
        "$kpm_root/tools/kptools" -l -i "$temporary" | tee "${temporary}.info"
        grep -qx 'patched=true' "${temporary}.info"
        rm -f -- "${temporary}.info"
        mv -- "$temporary" "$output"
        printf 'KPM-patched Image: %s\n' "$output"
        ;;
    *)
        echo "Usage: $0 build | patch Image Image.kpm" >&2
        exit 2
        ;;
esac
