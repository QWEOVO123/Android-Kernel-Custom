#include <linux/qweovo_bootmenu.h>
#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/file.h"
#include "linux/jump_label.h"
#include "linux/namei.h"
#include "linux/printk.h"
#include "linux/uaccess.h"
#include "selinux/selinux.h"
#include <asm/syscall.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/static_key.h>
#include "ksu.h"
#include "util.h"

#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/tp_marker.h"
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "feature/adb_root.h"
#include "preinit/preinit.h"

static int ksu_handle_init_mark_tracker(const char __user **filename_user)
{
    char path[64];
    unsigned long addr;
    const char __user *fn;
    long ret;

    if (unlikely(!filename_user))
        return 0;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    ret = strncpy_from_user(path, fn, sizeof(path));
    if (ret < 0)
        return 0;

    path[sizeof(path) - 1] = '\0';
    if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
        pr_info("hook_manager: escape to root for init executing ksud: %d\n", current->pid);
        escape_to_root_for_init();
    } else if (likely(strstr(path, "/app_process") == NULL && strstr(path, "/adbd") == NULL &&
                      strstr(path, "/stub_zygote") == NULL)) {
        pr_info("hook_manager: unmark %d exec %s\n", current->pid, path);
        ksu_clear_task_tracepoint_flag_if_needed(current);
    }

    return 0;
}

static long meow_su_redirect_path(int orig_nr, struct pt_regs *regs);

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
    if (qweovo_bootmenu_meow_su_enabled() &&
        qweovo_meow_su_uid_allowed(current_uid().val) &&
        !(PT_REGS_SYSCALL_PARM4(regs) & AT_EMPTY_PATH))
        return meow_su_redirect_path(orig_nr, (struct pt_regs *)regs);

    if (!qweovo_bootmenu_sukisu_enabled() || !ksu_su_compat_enabled)
        return ksu_syscall_table[orig_nr](regs);

    return ksu_handle_stat_sucompat(orig_nr, (struct pt_regs *)regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
    if (qweovo_bootmenu_meow_su_enabled() &&
        qweovo_meow_su_uid_allowed(current_uid().val))
        return meow_su_redirect_path(orig_nr, (struct pt_regs *)regs);

    if (!qweovo_bootmenu_sukisu_enabled() || !ksu_su_compat_enabled)
        return ksu_syscall_table[orig_nr](regs);

    return ksu_handle_faccessat_sucompat(orig_nr, (struct pt_regs *)regs);
}

/*
 * MeowSU: keep the whole su path in the kernel. An authorized app executes
 * the conventional su path; the syscall is redirected in place to a root
 * shell, so no su file, service, socket or mount is ever exposed.
 */
static const char *const meow_su_paths[] = {
    "/system/bin/su",
    "/system/xbin/su",
};
#define MEOW_SU_TARGET "/system/bin/sh"

extern int escape_with_meow_su_profile(void);

/* Reads argv[index]; returns 1 when present, 0 when absent, -EINVAL on fault. */
static int meow_su_read_arg(const char __user *const __user *argv, int index,
                            char *value, size_t size)
{
    const char __user *arg;
    long length;

    if (get_user(arg, argv + index) || !arg)
        return 0;
    length = strncpy_from_user(value, arg, size);
    if (length < 0)
        return -EINVAL;
    value[size - 1] = '\0';
    return 1;
}

/*
 * Only the forms a shell can serve are accepted:
 *   su                     -> interactive shell
 *   su -c CMD              -> command
 *   su 0|root [-c CMD]     -> command (the extra word is dropped)
 *   su -                   -> interactive shell
 * Anything else ("-v", "--version", "-h", "-mm", "su COMMAND") fails fast
 * with ENOENT instead of being mis-parsed by the shell or blocking on stdin.
 */
static int meow_su_parse_argv(const char __user *const __user *argv,
                              const char __user *const __user **adjusted)
{
    char first[32], next[32];
    int present;

    *adjusted = argv;
    if (!argv)
        return 0;

    present = meow_su_read_arg(argv, 1, first, sizeof(first));
    if (present < 0)
        return present;
    if (!present)
        return 0;

    if (!strcmp(first, "0") || !strcmp(first, "root")) {
        present = meow_su_read_arg(argv, 2, next, sizeof(next));
        if (present < 0)
            return present;
        if (!present)
            return 0;
        if (!strcmp(next, "-c") || !strcmp(next, "-")) {
            *adjusted = argv + 1;
            return 0;
        }
        return -EINVAL;
    }
    if (!strcmp(first, "-c")) {
        present = meow_su_read_arg(argv, 2, next, sizeof(next));
        return present > 0 ? 0 : -EINVAL;
    }
    if (!strcmp(first, "-"))
        return 0;
    if (first[0] == '-')
        return -EINVAL;
    return -EINVAL;
}

static int meow_su_open_target(void)
{
    const struct cred *old;
    struct file *file;
    int fd;

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0)
        return fd;
    old = override_creds(ksu_cred);
    file = filp_open(MEOW_SU_TARGET, O_PATH, 0);
    revert_creds(old);
    if (IS_ERR(file)) {
        put_unused_fd(fd);
        return PTR_ERR(file);
    }
    fd_install(fd, file);
    return fd;
}

static bool meow_su_path_match(const char *path)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(meow_su_paths); i++)
        if (!strcmp(path, meow_su_paths[i]))
            return true;
    return false;
}

static bool meow_su_requested(const char __user **filename_user,
                              const struct pt_regs *regs, bool execveat)
{
    char path[32];

    if (!filename_user || !*filename_user)
        return false;
    if (execveat &&
        ((int)PT_REGS_SYSCALL_PARM1(regs) != AT_FDCWD || (int)PT_REGS_PARM5(regs) != 0))
        return false;
    memset(path, 0, sizeof(path));
    if (strncpy_from_user(path, *filename_user, sizeof(path)) <= 0)
        return false;
    return meow_su_path_match(path);
}

/*
 * Granted apps may probe for su before using it. Only they see the path as
 * existing; every other app still gets ENOENT, so the artifact stays hidden.
 */
static long meow_su_redirect_path(int orig_nr, struct pt_regs *regs)
{
    const char __user **filename_user = (const char __user **)&PT_REGS_PARM2(regs);
    const char __user *replacement, *original;
    char path[32];
    long ret;

    if (!filename_user || !*filename_user)
        return ksu_syscall_table[orig_nr](regs);
    memset(path, 0, sizeof(path));
    if (strncpy_from_user(path, *filename_user, sizeof(path)) <= 0)
        return ksu_syscall_table[orig_nr](regs);
    if (!meow_su_path_match(path))
        return ksu_syscall_table[orig_nr](regs);
    replacement = userspace_stack_buffer(MEOW_SU_TARGET, sizeof(MEOW_SU_TARGET));
    if (!replacement)
        return ksu_syscall_table[orig_nr](regs);
    original = *filename_user;
    *filename_user = replacement;
    ret = ksu_syscall_table[orig_nr](regs);
    *filename_user = original;
    return ret;
}

static long meow_su_execve(const char __user **filename_user,
                           const char __user *const __user *argv_user,
                           int orig_nr, struct pt_regs *regs, bool execveat)
{
    const char __user *const __user *argv;
    const char __user *empty;
    unsigned long saved[5], envp;
    long ret;
    int fd;

    if (!meow_su_requested(filename_user, regs, execveat))
        return ksu_syscall_table[orig_nr](regs);

    envp = execveat ? PT_REGS_SYSCALL_PARM4(regs) : PT_REGS_PARM3(regs);
    if (meow_su_parse_argv(argv_user, &argv))
        return -ENOENT;
    fd = meow_su_open_target();
    if (fd < 0)
        return ksu_syscall_table[orig_nr](regs);
    empty = userspace_stack_buffer("", sizeof(""));
    if (!empty) {
        ksu_close_fd(fd);
        return ksu_syscall_table[orig_nr](regs);
    }
    if (escape_with_meow_su_profile()) {
        ksu_close_fd(fd);
        return ksu_syscall_table[orig_nr](regs);
    }

    saved[0] = PT_REGS_SYSCALL_PARM1(regs);
    saved[1] = regs->__PT_PARM2_REG;
    saved[2] = regs->__PT_PARM3_REG;
    saved[3] = regs->__PT_SYSCALL_PARM4_REG;
    saved[4] = regs->__PT_PARM5_REG;

    /* execveat(fd, "", argv, envp, AT_EMPTY_PATH) */
    PT_REGS_SYSCALL_PARM1(regs) = fd;
    regs->__PT_PARM2_REG = (unsigned long)empty;
    regs->__PT_PARM3_REG = (unsigned long)argv;
    regs->__PT_SYSCALL_PARM4_REG = envp;
    regs->__PT_PARM5_REG = AT_EMPTY_PATH;

    ret = ksu_syscall_table[__NR_execveat](regs);
    if (ret < 0) {
        PT_REGS_SYSCALL_PARM1(regs) = saved[0];
        regs->__PT_PARM2_REG = saved[1];
        regs->__PT_PARM3_REG = saved[2];
        regs->__PT_SYSCALL_PARM4_REG = saved[3];
        regs->__PT_PARM5_REG = saved[4];
        ksu_close_fd(fd);
    }
    return ret;
}

DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook()
{
    static_branch_disable(&ksud_execve_key);
}

static long __nocfi ksu_hook_execve_common(int orig_nr, const struct pt_regs *regs, bool execveat)
{
    const char __user **filename_user =
        execveat ? (const char __user **)&PT_REGS_PARM2(regs) : (const char __user **)&PT_REGS_SYSCALL_PARM1(regs);
    const char __user *const __user *argv_user = execveat ? (const char __user *const __user *)PT_REGS_PARM3(regs) :
                                                            (const char __user *const __user *)PT_REGS_PARM2(regs);
    bool current_is_init = is_init(current_cred());
    struct ksu_sulog_pending_event *pending_root_execve = NULL;
    long ret;

    if (static_branch_unlikely(&ksud_execve_key)) {
        if (execveat) {
            ksu_execveat_hook_ksud(regs);
        } else {
            ksu_execve_hook_ksud(regs);
        }
    }

    /*
     * Newer Android releases' init execs selinux_setup with execveat()
     * rather than execve(). Only an AT_EMPTY_PATH exec of an fd has no
     * path for preinit to swap.
     */
    if (unlikely(current->pid == 1) && (!execveat || !(PT_REGS_PARM5(regs) & AT_EMPTY_PATH)) &&
        ksu_preinit_handle_execve(filename_user, argv_user, orig_nr, (struct pt_regs *)regs, &ret))
        return ret;

    if (qweovo_bootmenu_meow_su_enabled() &&
        qweovo_meow_su_uid_allowed(current_uid().val))
        return meow_su_execve(filename_user, argv_user, orig_nr,
                              (struct pt_regs *)regs, execveat);

    if (!qweovo_bootmenu_sukisu_enabled())
        return ksu_syscall_table[orig_nr](regs);

    if (current_euid().val == 0)
        pending_root_execve = ksu_sulog_capture_root_execve(*filename_user, argv_user, GFP_KERNEL);

    if (current->pid != 1 && current_is_init) {
        ksu_handle_init_mark_tracker(filename_user);
        ret = execveat ? ksu_adb_root_handle_execveat((struct pt_regs *)regs) :
                         ksu_adb_root_handle_execve((struct pt_regs *)regs);
        if (ret) {
            pr_err("adb root failed: %ld\n", ret);
        }
    } else if (ksu_su_compat_enabled) {
        ret = execveat ? ksu_handle_execveat_sucompat(filename_user, orig_nr, (struct pt_regs *)regs) :
                         ksu_handle_execve_sucompat(filename_user, orig_nr, (struct pt_regs *)regs);
        ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
        return ret;
    }

    ret = ksu_syscall_table[orig_nr](regs);
    ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
    return ret;
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, false);
}

long __nocfi ksu_hook_execveat(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, true);
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
    uid_t old_uid = current_uid().val;
    long ret = ksu_syscall_table[orig_nr](regs);

    if (ret < 0)
        return ret;

    ksu_handle_setresuid(old_uid, current_uid().val);
    return ret;
}
