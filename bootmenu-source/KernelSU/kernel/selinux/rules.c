#include "linux/rcupdate.h"
#include "security.h"
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/lockdep.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/qweovo_bootmenu.h>

#include "uapi/selinux.h"
#include "klog.h" // IWYU pragma: keep
#include "selinux.h"
#include "sepolicy.h"
#include "ss/services.h"
#include "linux/lsm_audit.h" // IWYU pragma: keep
#include "xfrm.h"

struct selinux_policy *backup_sepolicy;

#define SELINUX_POLICY_INSTEAD_SELINUX_SS

#define ALL NULL

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
extern int avc_ss_reset(u32 seqno);
#else
extern int avc_ss_reset(struct selinux_avc *avc, u32 seqno);
#endif
// reset avc cache table, otherwise the new rules will not take effect if already denied
static void reset_avc_cache()
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
    avc_ss_reset(0);
    selnl_notify_policyload(0);
    selinux_status_update_policyload(0);
#else
    struct selinux_avc *avc = selinux_state.avc;
    avc_ss_reset(avc, 0);
    selnl_notify_policyload(0);
    selinux_status_update_policyload(&selinux_state, 0);
#endif
    selinux_xfrm_notify_policyload();
}

void apply_kernelsu_rules()
{
    struct selinux_policy *pol, *old_pol;
    struct policydb *db;

    if (!getenforce()) {
        pr_info("SELinux permissive or disabled, apply rules!\n");
    }

    mutex_lock(&selinux_state.policy_mutex);

    old_pol = rcu_dereference_protected(selinux_state.policy, lockdep_is_held(&selinux_state.policy_mutex));
    backup_sepolicy = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(backup_sepolicy)) {
        pr_err("failed to create backup sepolicy: %ld\n", PTR_ERR(backup_sepolicy));
        backup_sepolicy = NULL;
    } else {
        backup_sepolicy->sidtab = kzalloc(sizeof(*backup_sepolicy->sidtab), GFP_KERNEL);
        if (!backup_sepolicy->sidtab) {
            pr_err("failed to alloc backup sidtab\n");
            ksu_destroy_sepolicy(backup_sepolicy);
            backup_sepolicy = NULL;
        } else {
            int ret = policydb_load_isids(&backup_sepolicy->policydb, backup_sepolicy->sidtab);
            if (ret) {
                pr_err("failed to load isids for backup sepolicy: %d!\n", ret);
                kfree(backup_sepolicy->sidtab);
                ksu_destroy_sepolicy(backup_sepolicy);
                backup_sepolicy = NULL;
            } else {
                pr_info("backup sepolicy success! latest_granting=%d\n", backup_sepolicy->latest_granting);
            }
        }
    }
    pol = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(pol)) {
        pr_err("failed to dup selinux_policy: %ld\n", PTR_ERR(pol));
        goto out_unlock;
    }

    db = &pol->policydb;

    ksu_type(db, KERNEL_SU_DOMAIN, "domain");
    ksu_permissive(db, KERNEL_SU_DOMAIN);
    ksu_typeattribute(db, KERNEL_SU_DOMAIN, "mlstrustedsubject");
    ksu_typeattribute(db, KERNEL_SU_DOMAIN, "netdomain");
    ksu_typeattribute(db, KERNEL_SU_DOMAIN, "bluetoothdomain");

    // Create unconstrained file type
    ksu_type(db, KERNEL_SU_FILE, "file_type");
    ksu_typeattribute(db, KERNEL_SU_FILE, "mlstrustedobject");
    ksu_allow(db, "domain", KERNEL_SU_FILE, ALL, ALL);

    // allow all!
    ksu_allow(db, KERNEL_SU_DOMAIN, ALL, ALL, ALL);

    // allow us do any ioctl
    if (db->policyvers >= POLICYDB_VERSION_XPERMS_IOCTL) {
        ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "blk_file", ALL);
        ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "fifo_file", ALL);
        ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "chr_file", ALL);
        ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "file", ALL);
    }

    // our ksud triggered by init
    ksu_allow(db, "init", KERNEL_SU_DOMAIN, ALL, ALL);

    // copied from Magisk rules
    // suRights
    ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "dir", "search");
    ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "dir", "read");
    ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "file", "open");
    ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "file", "read");
    ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "process", "getattr");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "process", "sigchld");

    // allowLog
    ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "dir", "search");
    ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "read");
    ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "open");
    ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "getattr");

    // dumpsys, send fd
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "fd", "use");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "fifo_file", "write");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "fifo_file", "read");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "fifo_file", "open");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "fifo_file", "getattr");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "unix_stream_socket", "read");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "unix_stream_socket", "write");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "unix_stream_socket", "connectto");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "unix_stream_socket", "getopt");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "unix_stream_socket", "getattr");

    // use memfd created by su domain
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "memfd_file", "execute");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "memfd_file", "getattr");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "memfd_file", "map");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "memfd_file", "read");
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "memfd_file", "write");

    // bootctl
    ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "dir", "search");
    ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "file", "read");
    ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "file", "open");
    ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "process", "getattr");

    // Allow all binder transactions
    ksu_allow(db, "domain", KERNEL_SU_DOMAIN, "binder", ALL);

    // Allow system server kill su process
    ksu_allow(db, "system_server", KERNEL_SU_DOMAIN, "process", "getpgid");
    ksu_allow(db, "system_server", KERNEL_SU_DOMAIN, "process", "sigkill");

    rcu_assign_pointer(selinux_state.policy, pol);
    synchronize_rcu();
    ksu_destroy_sepolicy(old_pol);

    reset_avc_cache();
out_unlock:
    mutex_unlock(&selinux_state.policy_mutex);
}

/* Keep the service and its executable mapping separate from the SukiSU policy. */
bool apply_meow_tee_rules(void);
bool apply_meow_tee_rules(void)
{
    static const char domain[] = "meow_tee";
    static const char library[] = "meow_tee_file";
    struct selinux_policy *pol, *old_pol;
    struct policydb *db;
    bool ready = false;

    mutex_lock(&selinux_state.policy_mutex);
    old_pol = rcu_dereference_protected(selinux_state.policy,
                                       lockdep_is_held(&selinux_state.policy_mutex));
    if (!old_pol)
        goto out_unlock;
    pol = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(pol))
        goto out_unlock;
    db = &pol->policydb;

    if (!ksu_type(db, domain, "domain") ||
        !ksu_typeattribute(db, domain, "mlstrustedsubject") ||
        !ksu_permissive(db, domain) ||
        !ksu_type(db, library, "file_type") ||
        !ksu_allow(db, "init", domain, "process", "transition") ||
        !ksu_allow(db, domain, "shell_exec", "file", "entrypoint") ||
        !ksu_allow(db, "keystore", domain, "binder", "call") ||
        !ksu_allow(db, "appdomain", domain, "binder", "call") ||
        !ksu_allow(db, "keystore", domain, "fd", "use") ||
        !ksu_allow(db, "keystore", library, "file", "read") ||
        !ksu_allow(db, "keystore", library, "file", "map") ||
        !ksu_allow(db, "keystore", library, "file", "execute")) {
        ksu_destroy_sepolicy(pol);
        goto out_unlock;
    }

    ksu_allow(db, "init", domain, "process", "siginh");
    ksu_allow(db, "init", domain, "process", "rlimitinh");
    ksu_allow(db, "init", domain, "process", "sigkill");
    ksu_allow(db, "init", domain, "process", "getattr");
    ksu_allow(db, "keystore", domain, "binder", "transfer");
    ksu_allow(db, "keystore", domain, "unix_stream_socket", "connectto");
    ksu_allow(db, "keystore", domain, "unix_stream_socket", "read");
    ksu_allow(db, "keystore", domain, "unix_stream_socket", "write");
    ksu_allow(db, "keystore", domain, "unix_stream_socket", "getopt");
    ksu_allow(db, "keystore", library, "file", "getattr");
    ksu_allow(db, "servicemanager", domain, "binder", "call");
    ksu_allow(db, "servicemanager", domain, "binder", "transfer");
    ksu_allow(db, "servicemanager", domain, "process", "getattr");
    ksu_allow(db, "logd", domain, "dir", "search");
    ksu_allow(db, "logd", domain, "file", "open");
    ksu_allow(db, "logd", domain, "file", "read");
    ksu_allow(db, "logd", domain, "file", "getattr");

    rcu_assign_pointer(selinux_state.policy, pol);
    synchronize_rcu();
    ksu_destroy_sepolicy(old_pol);
    reset_avc_cache();
    ready = true;
out_unlock:
    mutex_unlock(&selinux_state.policy_mutex);
    pr_info("meow_tee: SELinux policy %s\n", ready ? "ready" : "unavailable");
    return ready;
}

bool apply_meow_manager_rules(void);
bool apply_meow_manager_rules(void)
{
    static const char domain[] = MEOW_MANAGER_DOMAIN;
    static const char sock[] = "meow_manager_socket";
    static const char uffd[] = "meow_manager_userfaultfd";
    static const struct {
        const char *source, *target, *class, *permission;
    } rules[] = {
        { "init", domain, "process", "transition" },
        { "init", domain, "process", "siginh" },
        { "init", domain, "process", "rlimitinh" },
        { "init", domain, "process", "sigkill" },
        { "init", domain, "process", "getattr" },
        { domain, "init", "process", "sigchld" },
        { domain, domain, "process", "getsched" },
        { domain, domain, "process", "setsched" },
        { domain, domain, "process", "fork" },
        { domain, domain, "process", "getattr" },
        { domain, domain, "process", "signal" },
        { domain, domain, "process", "getcap" },
        { domain, domain, "dir", "search" },
        { domain, domain, "dir", "getattr" },
        { domain, domain, "dir", "open" },
        { domain, domain, "dir", "read" },
        { domain, domain, "file", "getattr" },
        { domain, domain, "file", "open" },
        { domain, domain, "file", "read" },
        { domain, domain, "lnk_file", "read" },
        { domain, domain, "lnk_file", "getattr" },
        { domain, uffd, "anon_inode", "create" },
        { domain, uffd, "anon_inode", "ioctl" },
        { domain, uffd, "anon_inode", "read" },
        { domain, sock, "unix_stream_socket", "create" },
        { domain, sock, "unix_stream_socket", "bind" },
        { domain, sock, "unix_stream_socket", "listen" },
        { domain, sock, "unix_stream_socket", "accept" },
        { domain, sock, "unix_stream_socket", "getopt" },
        { domain, sock, "unix_stream_socket", "setopt" },
        { domain, sock, "unix_stream_socket", "read" },
        { domain, sock, "unix_stream_socket", "write" },
        { domain, sock, "unix_stream_socket", "getattr" },
        { domain, sock, "unix_stream_socket", "shutdown" },
        { domain, domain, "unix_dgram_socket", "create" },
        { domain, domain, "unix_dgram_socket", "connect" },
        { domain, domain, "unix_dgram_socket", "write" },
        { domain, domain, "unix_dgram_socket", "getattr" },
        { domain, domain, "unix_dgram_socket", "getopt" },
        { domain, domain, "unix_dgram_socket", "setopt" },
        { domain, "zygote_exec", "file", "entrypoint" },
        { domain, "zygote_exec", "file", "execute" },
        { domain, "zygote_exec", "file", "getattr" },
        { domain, "zygote_exec", "file", "map" },
        { domain, "zygote_exec", "file", "open" },
        { domain, "zygote_exec", "file", "read" },
        { domain, "tmpfs", "dir", "search" },
        { domain, "tmpfs", "dir", "getattr" },
        { domain, "tmpfs", "dir", "write" },
        { domain, "tmpfs", "dir", "add_name" },
        { domain, "tmpfs", "dir", "remove_name" },
        { domain, "tmpfs", "file", "open" },
        { domain, "tmpfs", "file", "read" },
        { domain, "tmpfs", "file", "map" },
        { domain, "tmpfs", "file", "getattr" },
        { domain, "dalvik_runtime_prop", "file", "open" },
        { domain, "dalvik_runtime_prop", "file", "read" },
        { domain, "dalvik_runtime_prop", "file", "getattr" },
        { domain, "dalvik_runtime_prop", "file", "map" },
        { domain, "odsign_prop", "file", "open" },
        { domain, "odsign_prop", "file", "read" },
        { domain, "odsign_prop", "file", "getattr" },
        { domain, "odsign_prop", "file", "map" },
        { domain, "logdw_socket", "sock_file", "write" },
        { domain, "logd", "unix_dgram_socket", "sendto" },
        { domain, "binder_device", "chr_file", "open" },
        { domain, "binder_device", "chr_file", "read" },
        { domain, "binder_device", "chr_file", "write" },
        { domain, "binder_device", "chr_file", "getattr" },
        { domain, "binder_device", "chr_file", "map" },
        { domain, "binder_device", "chr_file", "ioctl" },
        { domain, "binder_device", "lnk_file", "read" },
        { domain, "binderfs", "dir", "search" },
        { domain, "package_service", "service_manager", "find" },
        { domain, "servicemanager", "binder", "call" },
        { domain, "servicemanager", "binder", "transfer" },
        { "servicemanager", domain, "binder", "transfer" },
        { domain, "system_server", "binder", "call" },
        { domain, "system_server", "binder", "transfer" },
        { domain, sock, "sock_file", "create" },
        { domain, sock, "sock_file", "getattr" },
        { domain, sock, "sock_file", "setattr" },
        { domain, sock, "sock_file", "unlink" },
        { "appdomain", "tmpfs", "dir", "search" },
        { "appdomain", sock, "sock_file", "write" },
        { "appdomain", sock, "unix_stream_socket", "connectto" },
    };
    struct selinux_policy *pol, *old_pol;
    struct policydb *db;
    size_t i;
    bool ready = false;

    mutex_lock(&selinux_state.policy_mutex);
    old_pol = rcu_dereference_protected(selinux_state.policy,
                                       lockdep_is_held(&selinux_state.policy_mutex));
    if (!old_pol)
        goto out;
    pol = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(pol))
        goto out;
    db = &pol->policydb;
    if (!ksu_type(db, domain, "domain") || !ksu_typeattribute(db, domain, "appdomain") ||
        !ksu_type(db, sock, "file_type") ||
        !ksu_typeattribute(db, sock, "mlstrustedobject") ||
        !ksu_typeattribute(db, sock, "mlstrustedsubject") ||
        !ksu_type_transition(db, domain, domain, "unix_stream_socket", sock, NULL) ||
        !ksu_type(db, uffd, "file_type") ||
        !ksu_type_transition(db, domain, domain, "anon_inode", uffd, "[userfaultfd]") ||
        !ksu_type_transition(db, domain, "tmpfs", "sock_file", sock, "control"))
        goto discard;
    for (i = 0; i < ARRAY_SIZE(rules); i++) {
        if (!ksu_allow(db, rules[i].source, rules[i].target, rules[i].class, rules[i].permission))
            goto discard;
    }
    rcu_assign_pointer(selinux_state.policy, pol);
    synchronize_rcu();
    ksu_destroy_sepolicy(old_pol);
    reset_avc_cache();
    ready = true;
    goto out;
discard:
    ksu_destroy_sepolicy(pol);
out:
    mutex_unlock(&selinux_state.policy_mutex);
    if (ready)
        ready = selinux_meow_manager_cache_sid();
    pr_info("meow_manager: SELinux policy %s\n", ready ? "ready" : "unavailable");
    return ready;
}

bool apply_meow_su_rules(void);
extern void meow_su_cache_sid(void);
bool apply_meow_su_rules(void)
{
    static const char domain[] = "meow_shell";
    struct selinux_policy *pol, *old_pol;
    struct policydb *db;
    bool ready = false;

    mutex_lock(&selinux_state.policy_mutex);
    old_pol = rcu_dereference_protected(selinux_state.policy,
                                       lockdep_is_held(&selinux_state.policy_mutex));
    if (!old_pol)
        goto out;
    pol = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(pol))
        goto out;
    db = &pol->policydb;
    /*
     * Deliberate permissive root-shell domain. It is only entered by
     * escape_with_meow_su_profile(), after the kernel matched the caller's
     * uid against the in-memory MeowSU grant table. No service, socket or
     * helper executable takes part in this path any more.
     *
     * Deliberately NOT named "ksu" and with no wildcard allow, no
     * typeattribute writes and no ioctl xperm rules: detection engines read
     * the live policy and treat "new type + wildcard grants + attribute
     * writes" as a KernelSU fingerprint. The permissive bit already lets
     * every check whose source is this domain pass.
     */
    if (!ksu_type(db, domain, "domain")) {
        ksu_destroy_sepolicy(pol);
        goto out;
    }
    ksu_permissive(db, domain);
    /*
     * Cross-domain rules. Every check below has the other domain as source,
     * so our own permissive flag does not apply; without them a service
     * handing us a binder or an fd, or system_server killing the shell, is
     * denied by its own enforcing policy. Specific permissions only, so the
     * policy does not grow a wildcard-grant signature.
     */
    ksu_allow(db, "servicemanager", domain, "dir", "search");
    ksu_allow(db, "servicemanager", domain, "dir", "read");
    ksu_allow(db, "servicemanager", domain, "file", "open");
    ksu_allow(db, "servicemanager", domain, "file", "read");
    ksu_allow(db, "servicemanager", domain, "process", "getattr");
    ksu_allow(db, "hwservicemanager", domain, "dir", "search");
    ksu_allow(db, "hwservicemanager", domain, "file", "read");
    ksu_allow(db, "hwservicemanager", domain, "file", "open");
    ksu_allow(db, "hwservicemanager", domain, "process", "getattr");
    ksu_allow(db, "logd", domain, "dir", "search");
    ksu_allow(db, "logd", domain, "file", "read");
    ksu_allow(db, "logd", domain, "file", "open");
    ksu_allow(db, "logd", domain, "file", "getattr");
    ksu_allow(db, "domain", domain, "process", "sigchld");
    ksu_allow(db, "domain", domain, "fd", "use");
    ksu_allow(db, "domain", domain, "fifo_file", "write");
    ksu_allow(db, "domain", domain, "fifo_file", "read");
    ksu_allow(db, "domain", domain, "fifo_file", "open");
    ksu_allow(db, "domain", domain, "fifo_file", "getattr");
    ksu_allow(db, "domain", domain, "unix_stream_socket", "read");
    ksu_allow(db, "domain", domain, "unix_stream_socket", "write");
    ksu_allow(db, "domain", domain, "unix_stream_socket", "connectto");
    ksu_allow(db, "domain", domain, "unix_stream_socket", "getopt");
    ksu_allow(db, "domain", domain, "unix_stream_socket", "getattr");
    ksu_allow(db, "domain", domain, "memfd_file", "execute");
    ksu_allow(db, "domain", domain, "memfd_file", "getattr");
    ksu_allow(db, "domain", domain, "memfd_file", "map");
    ksu_allow(db, "domain", domain, "memfd_file", "read");
    ksu_allow(db, "domain", domain, "memfd_file", "write");
    ksu_allow(db, "domain", domain, "binder", "call");
    ksu_allow(db, "domain", domain, "binder", "transfer");
    ksu_allow(db, "system_server", domain, "process", "getpgid");
    ksu_allow(db, "system_server", domain, "process", "sigkill");

    rcu_assign_pointer(selinux_state.policy, pol);
    synchronize_rcu();
    ksu_destroy_sepolicy(old_pol);
    reset_avc_cache();
    ready = true;
out:
    mutex_unlock(&selinux_state.policy_mutex);
    if (ready)
        meow_su_cache_sid();
    pr_info("meow_su: SELinux policy %s\n", ready ? "ready" : "unavailable");
    return ready;
}

#define KSU_SEPOLICY_MAX_BATCH_SIZE (8U * 1024U * 1024U)
#define KSU_SEPOLICY_MAX_ARGS 5

struct sepol_data {
    u32 cmd;
    u32 subcmd;
};

struct sepol_batch_cursor {
    const u8 *cur;
    const u8 *end;
};

static size_t sepol_remaining(const struct sepol_batch_cursor *cursor)
{
    return (size_t)(cursor->end - cursor->cur);
}

static int sepol_read_cmd_header(struct sepol_batch_cursor *cursor, struct sepol_data *header)
{
    if (sepol_remaining(cursor) < sizeof(*header)) {
        return -EINVAL;
    }

    memcpy(header, cursor->cur, sizeof(*header));
    cursor->cur += sizeof(*header);

    return 0;
}

static int sepol_read_string(struct sepol_batch_cursor *cursor, const char **out)
{
    u32 len;
    const char *str;

    if (sepol_remaining(cursor) < sizeof(len)) {
        return -EINVAL;
    }

    memcpy(&len, cursor->cur, sizeof(len));
    cursor->cur += sizeof(len);

    if (len >= sepol_remaining(cursor)) {
        return -EINVAL;
    }

    str = (const char *)cursor->cur;
    if (memchr(str, '\0', len) != NULL || str[len] != '\0') {
        return -EINVAL;
    }

    cursor->cur += len + 1;
    if (len == 0) {
        *out = ALL;
        return 0;
    }

    *out = str;
    return 0;
}

static int sepol_require_not_all(const char *value, const char *name)
{
    if (value != ALL) {
        return 0;
    }

    pr_err("sepol: %s cannot be ALL.\n", name);
    return -EINVAL;
}

static int sepol_expected_argc(u32 cmd)
{
    switch (cmd) {
    case KSU_SEPOLICY_CMD_NORMAL_PERM:
        return 4;
    case KSU_SEPOLICY_CMD_XPERM:
        return 5;
    case KSU_SEPOLICY_CMD_TYPE_STATE:
        return 1;
    case KSU_SEPOLICY_CMD_TYPE:
    case KSU_SEPOLICY_CMD_TYPE_ATTR:
        return 2;
    case KSU_SEPOLICY_CMD_ATTR:
        return 1;
    case KSU_SEPOLICY_CMD_TYPE_TRANSITION:
        return 5;
    case KSU_SEPOLICY_CMD_TYPE_CHANGE:
        return 4;
    case KSU_SEPOLICY_CMD_GENFSCON:
        return 3;
    default:
        return -EINVAL;
    }
}

static int apply_one_sepolicy_cmd(struct policydb *db, const struct sepol_data *header, const char **args)
{
    bool success = false;
    int ret;

    switch (header->cmd) {
    case KSU_SEPOLICY_CMD_NORMAL_PERM:
        if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW) {
            success = ksu_allow(db, args[0], args[1], args[2], args[3]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY) {
            success = ksu_deny(db, args[0], args[1], args[2], args[3]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW) {
            success = ksu_auditallow(db, args[0], args[1], args[2], args[3]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT) {
            success = ksu_dontaudit(db, args[0], args[1], args[2], args[3]);
        } else {
            pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
        }
        return success ? 0 : -EINVAL;

    case KSU_SEPOLICY_CMD_XPERM:
        ret = sepol_require_not_all(args[3], "operation");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[4], "perm_set");
        if (ret < 0) {
            return ret;
        }

        if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_ALLOW) {
            success = ksu_allowxperm(db, args[0], args[1], args[2], args[4]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW) {
            success = ksu_auditallowxperm(db, args[0], args[1], args[2], args[4]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT) {
            success = ksu_dontauditxperm(db, args[0], args[1], args[2], args[4]);
        } else {
            pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
        }
        return success ? 0 : -EINVAL;

    case KSU_SEPOLICY_CMD_TYPE_STATE:
        ret = sepol_require_not_all(args[0], "type");
        if (ret < 0) {
            return ret;
        }

        if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE) {
            success = ksu_permissive(db, args[0]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE) {
            success = ksu_enforce(db, args[0]);
        } else {
            pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
        }
        return success ? 0 : -EINVAL;

    case KSU_SEPOLICY_CMD_TYPE:
    case KSU_SEPOLICY_CMD_TYPE_ATTR:
        ret = sepol_require_not_all(args[0], "type");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[1], "attribute");
        if (ret < 0) {
            return ret;
        }

        if (header->cmd == KSU_SEPOLICY_CMD_TYPE) {
            success = ksu_type(db, args[0], args[1]);
        } else {
            success = ksu_typeattribute(db, args[0], args[1]);
        }
        if (!success) {
            pr_err("sepol: %d failed.\n", header->cmd);
            return -EINVAL;
        }
        return 0;

    case KSU_SEPOLICY_CMD_ATTR:
        ret = sepol_require_not_all(args[0], "attribute");
        if (ret < 0) {
            return ret;
        }

        if (!ksu_attribute(db, args[0])) {
            pr_err("sepol: %d failed.\n", header->cmd);
            return -EINVAL;
        }
        return 0;

    case KSU_SEPOLICY_CMD_TYPE_TRANSITION: {
        const char *object = ALL;

        ret = sepol_require_not_all(args[0], "src");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[1], "tgt");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[2], "cls");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[3], "default_type");
        if (ret < 0) {
            return ret;
        }

        object = args[4];

        success = ksu_type_transition(db, args[0], args[1], args[2], args[3], object);
        return success ? 0 : -EINVAL;
    }

    case KSU_SEPOLICY_CMD_TYPE_CHANGE:
        ret = sepol_require_not_all(args[0], "src");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[1], "tgt");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[2], "cls");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[3], "default_type");
        if (ret < 0) {
            return ret;
        }

        if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE) {
            success = ksu_type_change(db, args[0], args[1], args[2], args[3]);
        } else if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER) {
            success = ksu_type_member(db, args[0], args[1], args[2], args[3]);
        } else {
            pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
        }
        return success ? 0 : -EINVAL;

    case KSU_SEPOLICY_CMD_GENFSCON:
        ret = sepol_require_not_all(args[0], "name");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[1], "path");
        if (ret < 0) {
            return ret;
        }
        ret = sepol_require_not_all(args[2], "context");
        if (ret < 0) {
            return ret;
        }

        if (!ksu_genfscon(db, args[0], args[1], args[2])) {
            pr_err("sepol: %d failed.\n", header->cmd);
            return -EINVAL;
        }
        return 0;

    default:
        pr_err("sepol: unknown cmd: %d\n", header->cmd);
        return -EINVAL;
    }
}

int handle_sepolicy(void __user *user_data, u64 data_len)
{
    struct selinux_policy *pol, *old_pol;
    struct policydb *db;
    struct sepol_batch_cursor cursor;
    u8 *payload;
    int ret;
    int success_cmd_count;
    u32 cmd_index;

    if (qweovo_bootmenu_selinux_locked()) {
        qweovo_security_audit("policy-edit", -EPERM);
        return -EPERM;
    }
    if (!user_data || !data_len) {
        return -EINVAL;
    }

    if (data_len > KSU_SEPOLICY_MAX_BATCH_SIZE) {
        return -E2BIG;
    }

    payload = kvmalloc((size_t)data_len, GFP_KERNEL);
    if (!payload) {
        return -ENOMEM;
    }

    if (copy_from_user(payload, user_data, (size_t)data_len)) {
        ret = -EFAULT;
        goto out_free;
    }

    if (!getenforce()) {
        pr_info("SELinux permissive or disabled when handle policy!\n");
    }

    mutex_lock(&selinux_state.policy_mutex);

    old_pol = rcu_dereference_protected(selinux_state.policy, lockdep_is_held(&selinux_state.policy_mutex));
    pol = ksu_dup_sepolicy(old_pol);
    if (IS_ERR(pol)) {
        ret = PTR_ERR(pol);
        pr_err("ksu_dup_sepolicy err: %d\n", ret);
        goto out_unlock;
    }
    db = &pol->policydb;

    cursor.cur = payload;
    cursor.end = payload + (size_t)data_len;

    ret = 0;
    success_cmd_count = 0;
    cmd_index = 0;
    while (cursor.cur < cursor.end) {
        struct sepol_data header;
        const char *args[KSU_SEPOLICY_MAX_ARGS] = { 0 };
        int expected_argc;
        u32 arg_index;

        ret = sepol_read_cmd_header(&cursor, &header);
        if (ret < 0) {
            pr_err("sepol: failed to read cmd header #%u.\n", cmd_index);
            goto out_drop_new_policy;
        }

        expected_argc = sepol_expected_argc(header.cmd);
        if (expected_argc < 0 || expected_argc > KSU_SEPOLICY_MAX_ARGS) {
            ret = -EINVAL;
            pr_err("sepol: invalid cmd header #%u.\n", cmd_index);
            goto out_drop_new_policy;
        }

        for (arg_index = 0; arg_index < (u32)expected_argc; arg_index++) {
            ret = sepol_read_string(&cursor, &args[arg_index]);
            if (ret < 0) {
                pr_err("sepol: failed to read cmd #%u arg #%u.\n", cmd_index, arg_index);
                goto out_drop_new_policy;
            }
        }

        ret = apply_one_sepolicy_cmd(db, &header, args);
        if (ret < 0) {
            pr_err("sepol: cmd #%u failed, cmd=%u subcmd=%u.\n", cmd_index, header.cmd, header.subcmd);
        } else {
            success_cmd_count++;
        }
        cmd_index++;
    }

    rcu_assign_pointer(selinux_state.policy, pol);
    synchronize_rcu();
    ksu_destroy_sepolicy(old_pol);

    reset_avc_cache();
    ret = success_cmd_count;
    goto out_unlock;

out_drop_new_policy:
    ksu_destroy_sepolicy(pol);
out_unlock:
    mutex_unlock(&selinux_state.policy_mutex);
out_free:
    kvfree(payload);

    return ret;
}
