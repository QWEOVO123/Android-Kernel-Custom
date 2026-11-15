"""Compile the merged ARM64 kernel using the matching Android toolchain."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time
import argparse
import re

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--kernel', type=Path, required=True)
parser.add_argument('--clang-bin', type=Path, required=True)
parser.add_argument('--out', type=Path, default=repo / 'out/kernel')
parser.add_argument('--dest', type=Path, default=repo / 'out/artifacts')
parser.add_argument('--config', type=Path, default=repo / 'config/tb320fc-17.9.config')
parser.add_argument('--jobs', type=int, default=min(6, os.cpu_count() or 1))
parser.add_argument('--ramdump', action=argparse.BooleanOptionalAction, default=True)
parser.add_argument('--tee-payload', type=Path)
parser.add_argument('--sukisu-public-key', type=Path, help='RSA-2048 public key, SPKI DER')
parser.add_argument('--manager-payload', type=Path, required=True, help='DEX jar for the non-root Manager broker')
parser.add_argument('--su-client', type=Path, help='Android ARM64 su executable for MeowSU mode')
parser.add_argument('--build-timestamp', default='',
                    help='override KBUILD_BUILD_TIMESTAMP, e.g. "Mon Oct 20 05:29:25 UTC 2025"; empty = real time')
parser.add_argument('--build-user', default='meow', help='KBUILD_BUILD_USER shown in /proc/version')
parser.add_argument('--build-host', default='linux', help='KBUILD_BUILD_HOST shown in /proc/version')
parser.add_argument('--build-version', default='', help='KBUILD_BUILD_VERSION, i.e. the #N build number')
parser.add_argument('--release', default='5.10.233-android12-9-Bootmenu-byQWEOVO',
                    help='kernel release/localversion string reported by uname -r')
opts = parser.parse_args()
assert opts.release.startswith('5.10.233'), 'release must keep the 5.10.233 base version'
assert not opts.build_version or opts.build_version.isdigit()
if opts.jobs < 1:
    parser.error('--jobs must be positive')
release = opts.release
kernel = opts.kernel.resolve(strict=True)
tc = opts.clang_bin.resolve(strict=True)
out = opts.out.resolve()
dest = opts.dest.resolve()
if out.exists() or dest.exists() or out == dest or out in dest.parents or dest in out.parents:
    parser.error('--out and --dest must be separate fresh directories')
out.mkdir(parents=True)
config = out / '.config'
shutil.copyfile(opts.config, config)
env = os.environ.copy()
env['PATH'] = str(tc) + ':' + env['PATH']
env['KBUILD_BUILD_USER'] = opts.build_user
env['KBUILD_BUILD_HOST'] = opts.build_host
if opts.build_timestamp:
    env['KBUILD_BUILD_TIMESTAMP'] = opts.build_timestamp
if opts.build_version:
    env['KBUILD_BUILD_VERSION'] = opts.build_version
args = ['make', '-C', str(kernel), f'O={out}', 'ARCH=arm64', 'LLVM=1', 'LLVM_IAS=1',
        'CROSS_COMPILE=aarch64-linux-gnu-', 'LOCALVERSION=', 'PYTHON=python3', 'KCFLAGS=-D__ANDROID_COMMON_KERNEL__',
        'KSU_GITHUB_VER=4.2.1-r2', 'GITHUB_COMMITS=3913', 'CURL_BIN=/bin/true']
if opts.tee_payload:
    payload = opts.tee_payload.resolve(strict=True)
    if not payload.is_file() or not re.fullmatch(r'[/A-Za-z0-9_.-]+', str(payload)):
        parser.error('--tee-payload must point to a file with a shell-safe path')
    args.append(f'QWEOVO_TEE_PAYLOAD={payload}')
manager_payload = opts.manager_payload.resolve(strict=True)
if not manager_payload.is_file() or not re.fullmatch(r'[/A-Za-z0-9_.-]+', str(manager_payload)):
    parser.error('--manager-payload must point to a file with a shell-safe path')
args.append(f'QWEOVO_MANAGER_PAYLOAD={manager_payload}')
if opts.su_client:
    su_client = opts.su_client.resolve(strict=True)
    if not su_client.is_file() or not re.fullmatch(r'[/A-Za-z0-9_.-]+', str(su_client)):
        parser.error('--su-client must point to a file with a shell-safe path')
    args.append(f'QWEOVO_SU_CLIENT={su_client}')
authorization_key_id = None
if opts.sukisu_public_key:
    public = opts.sukisu_public_key.resolve(strict=True)
    rsa = ['openssl', 'rsa', '-pubin', '-inform', 'DER', '-in', str(public)]
    details = subprocess.check_output(rsa + ['-noout', '-text'], text=True)
    if '2048 bit' not in details or 'Exponent: 65537 ' not in details:
        parser.error('SukiSU authorization requires a 2048-bit RSA public key with exponent 65537')
    key_der = subprocess.check_output(rsa + ['-RSAPublicKey_out', '-outform', 'DER'])
    authorization_key_id = hashlib.sha256(key_der).hexdigest()
    key_header = out / 'qweovo_sukisu_public_key.h'
    rows = [', '.join(f'0x{byte:02x}' for byte in key_der[i:i + 12])
            for i in range(0, len(key_der), 12)]
    key_header.write_text('#define QWEOVO_SUKISU_PUBLIC_KEY 1\n'
                          'static const unsigned char sukisu_public_key[] = {\n\t'
                          + ',\n\t'.join(rows) + '\n};\n')
    args.append(f'QWEOVO_SUKISU_KEY_HEADER={key_header}')
subprocess.run([str(kernel / 'scripts/config'), '--file', str(config), '-d', 'KPM',
                '-d', 'LTO_NONE', '-d', 'LTO_CLANG_FULL', '-e', 'LTO_CLANG_THIN',
                '-e', 'CFI_CLANG', '-e', 'PROCESS_UID_NICE_LOCK', '-e', 'QWEOVO_BOOT_MENU', '-d', 'LOCALVERSION_AUTO', '--set-str', 'LOCALVERSION',
                release.removeprefix('5.10.233')], check=True)
subprocess.run([str(kernel / 'scripts/config'), '--file', str(config),
                '-e' if opts.ramdump else '-d', 'QWEOVO_QCOM_PANIC_RAMDUMP'], check=True)
log = out / 'build.log'
start = time.time()
with log.open('a') as f:
    subprocess.run(args + ['olddefconfig'], env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
    content = config.read_text()
    for key in ('KSU', 'KSU_MANUAL_SU', 'CPU_PUBLIC_CONTROL', 'PROCESS_PUBLIC_CONTROL',
                'IP_FORWARD_TTL_PUBLIC_CONTROL', 'PROCESS_UID_NICE_LOCK', 'QWEOVO_BOOT_MENU', 'LTO_CLANG_THIN', 'CFI_CLANG', 'MODVERSIONS'):
        assert f'CONFIG_{key}=y\n' in content, key
    assert '# CONFIG_KPM is not set\n' in content
    if opts.ramdump:
        assert 'CONFIG_QWEOVO_QCOM_PANIC_RAMDUMP=y\n' in content
    if authorization_key_id:
        for key in ('ASYMMETRIC_PUBLIC_KEY_SUBTYPE', 'CRYPTO_RSA', 'CRYPTO_SHA256'):
            assert f'CONFIG_{key}=y\n' in content, key
    shutil.copy2(config, out / 'config.before-build')
    subprocess.run(args + ['headers_install'], env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
    print('Configuration verified: ThinLTO / CFI / SukiSU / public interfaces ON, KPM OFF', flush=True)
    print('Compiling with Android Clang, -j' + str(opts.jobs) + '; log: ' + str(log), flush=True)
    result = subprocess.run(args + [f'-j{opts.jobs}', 'Image', 'modules'], env=env, stdout=f, stderr=subprocess.STDOUT)
if result.returncode:
    print('\n'.join(log.read_text(errors='replace').splitlines()[-90:]), flush=True)
    raise SystemExit(result.returncode)
image = out / 'arch/arm64/boot/Image'
payload = image.read_bytes()
assert payload[56:60] == b'ARM\x64'
actual_release = (out / 'include/config/kernel.release').read_text().strip()
assert actual_release == release, actual_release
assert '# CONFIG_KPM is not set\n' in config.read_text()
report = {
    'kernel_release': release, 'image': str(image), 'bytes': len(payload),
    'image_sha256': hashlib.sha256(payload).hexdigest(),
    'compiler': subprocess.check_output([str(tc / 'clang'), '--version'], env=env, text=True).splitlines()[0],
    'build_timestamp': opts.build_timestamp or 'real-time',
    'build_user': opts.build_user, 'build_host': opts.build_host,
    'build_version': opts.build_version or 'auto',
    'kpm_enabled': False, 'kpm_injected': False,
    'panic_ramdump_hook': opts.ramdump,
    'sukisu_authorization_key_sha256': authorization_key_id,
    'sukisu_build_version': '41098 / v4.2.1-r2 (matching prior build; no live version lookup)',
    'elapsed_seconds': round(time.time() - start), 'log': str(log), 'device_boot_tested': False,
}
report_file = out / 'BUILD_INFO.json'
report_file.write_text(json.dumps(report, indent=2) + '\n')
dest.mkdir(parents=True)
for src, name in [(image, 'Image'), (config, 'kernel.config'), (log, 'build.log'),
                  (report_file, 'BUILD_INFO.json'), (out / 'Module.symvers', 'Module.symvers'),
                  (out / 'System.map', 'System.map'), (out / 'modules.order', 'modules.order')]:
    shutil.copy2(src, dest / name)
(dest / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in sorted(dest.iterdir()) if p.is_file()))
print(json.dumps(report, indent=2), flush=True)
print('Exported artifacts: ' + str(dest), flush=True)
