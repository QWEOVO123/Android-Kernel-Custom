#!/system/bin/sh
set -eu

base=/dev/.meow-tee
keybox=/data/keymaster/keybox.xml
[ -d "$base" ] || exit 0

# A file manager may have copied the keybox in as an app-owned 0777 entry.
# This runs as root on post-fs-data, before the SELinux boot lock activates,
# so repair ownership/mode instead of silently skipping the whole simulation.
if [ -d /data/keymaster ] && [ ! -L /data/keymaster ]; then
    if [ "$(stat -c '%u:%g:%a' /data/keymaster)" != "0:0:700" ]; then
        chown root:root /data/keymaster 2>/dev/null || true
        chmod 700 /data/keymaster 2>/dev/null || true
    fi
    if [ -f "$keybox" ] && [ ! -L "$keybox" ]; then
        if [ "$(stat -c '%u:%g:%a' "$keybox")" != "0:0:600" ]; then
            chown root:root "$keybox" 2>/dev/null || true
            chmod 600 "$keybox" 2>/dev/null || true
        fi
    fi
fi

if [ -L /data/keymaster ] || [ -L "$keybox" ] || [ ! -f "$keybox" ] ||
   [ ! -s "$keybox" ] ||
   [ "$(stat -c '%u:%g:%a' /data/keymaster)" != "0:0:700" ] ||
   [ "$(stat -c '%u:%g:%a' "$keybox")" != "0:0:600" ]; then
    log -t MeowTEE "Skipping startup: need a root-only /data/keymaster/keybox.xml"
    exit 0
fi

# Avoid attaching two copies of the interceptor to the same keystore.
module=/data/adb/modules/tricky_store
if [ -f "$module/module.prop" ] && [ ! -e "$module/disable" ] &&
   [ ! -e "$module/remove" ]; then
    log -t MeowTEE "Skipping startup: disable the standalone TEESimulator module"
    exit 0
fi

umask 077
cd "$base"
/system/bin/tar -xzf payload.tar.gz
chmod 0400 service.jar
/system/bin/chcon u:object_r:meow_tee_file:s0 libTEESimulator.so
exec /system/bin/app_process -Djava.class.path="$base/service.jar" "$base" \
    --nice-name=MeowTEESimulator org.matrix.TEESimulator.App --meow-boot
