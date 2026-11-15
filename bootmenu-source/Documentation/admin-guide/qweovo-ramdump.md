# QWEOVO public panic policy and cape USB ramdump

Enable `CONFIG_QWEOVO_QCOM_PANIC_RAMDUMP=y` for the public proc interfaces and
the verified TB320FC/cape Qualcomm firmware path. KPM remains disabled.

## Public App interfaces

- `/proc/panic_ctl` (0666): read or write `oops_continue=0|1` and
  `panic_action=reboot|ramdump`. Both options are validated before applying.
  Default: no best-effort Oops continuation, normal crash reboot.
- `/proc/qweovo_info` (0444): schema-1 UTF-8 JSON with stable kernel identity
  `qweovo.secret_kernel`, build release, capabilities, interface paths,
  configuration types/defaults/current values and readiness.

Ordinary Android Apps may access these nodes without su or extra permissions.
SELinux exceptions are limited to these private proc markers, using the safe
proc inode accessor. SELinux remains Enforcing. See
`app-api/public-kernel-api.md` and `app-api/QweovoKernelApi.kt` for the full ABI.

## Runtime and boot settings

Runtime settings last until reboot. Boot command-line overrides:

```text
qweovo_oops_continue=1 qweovo_panic_action=ramdump
```

`oops_continue=1` maps to native `panic_on_oops=0`; it does not make fatal
`panic()` return. The two exposed panic actions always restart: positive
native panic_timeout delays are retained, zero is changed to immediate restart.

## Qualcomm handoff

The core initcall resolves DT resources and verifies the atomic SCM IO-write
interface. The existing vendor download-mode module is loaded with its normal
`download_mode=1` parameter to retain SDI support. After successful module init,
the temporary hardware download request is cleared. Selecting an action from
userspace only updates policy and does not arm a dump or trigger a fault.

After vendor panic notifiers finish, reboot policy clears DLOAD again and
selects a cold crash reset. Ramdump policy sets the USB/QPST destination, arms
full dump (0x10) with the vendor SCM wire ABI, and selects warm reset to XBL.
Failed dump requests fall back to ordinary reboot. This path allocates no
memory and takes no sleeping locks. Normal reboot keeps the vendor handler.

`qweovo_ramdump=0` disables hardware setup and module argument injection, while
the identity and policy APIs remain available. Runtime Ramdump selection then
fails with EOPNOTSUPP. Failures before core initialization, firmware-only resets,
watchdogs bypassing panic and power loss are outside this hook's coverage.

`ramdump_ready` reports prepared kernel/firmware interfaces, not proof of a
successful crash capture. Preserve the exact build's vmlinux, System.map and
configuration. The previous always-dump build completed a Sahara RAM capture;
the public-policy build was tested without deliberately triggering a fault.
