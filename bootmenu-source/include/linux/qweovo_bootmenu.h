/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_QWEOVO_BOOTMENU_H
#define _LINUX_QWEOVO_BOOTMENU_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/uidgid.h>

struct inode;
struct file;

#define MEOW_MANAGER_UID 5998
#define MEOW_MANAGER_DOMAIN "meow_manager"

#ifdef CONFIG_QWEOVO_BOOT_MENU
bool qweovo_bootmenu_sukisu_enabled(void);
bool qweovo_bootmenu_meow_su_enabled(void);
bool qweovo_meow_su_grants_inode(const struct inode *inode);
bool qweovo_meow_su_uid_allowed(u32 uid);
int qweovo_meow_su_create_nodes(void);
bool qweovo_bootmenu_custom_enabled(void);
bool qweovo_bootmenu_status_spoofer_enabled(void);
bool qweovo_bootmenu_tee_enabled(void);
bool qweovo_bootmenu_module_lock_requested(void);
bool qweovo_bootmenu_selinux_locked(void);
bool qweovo_bootmenu_selinux_lock_requested(void);
bool qweovo_bootmenu_selinux_lock_writer(const struct inode *inode);
#ifdef CONFIG_SECURITY_SELINUX
int qweovo_selinux_activate_boot_lock(void);
#endif
bool qweovo_bootmenu_audit_enabled(void);
bool qweovo_security_audit_available(void);
void qweovo_security_audit_prepare(void);
bool qweovo_security_audit_inode(const struct inode *inode);
void qweovo_security_audit(const char *event, int result);
void qweovo_security_pipe_merge_blocked(void);
u64 qweovo_security_pipe_merge_count(void);
bool qweovo_bootmenu_committed(void);
int qweovo_bootmenu_install_fd(void);
void qweovo_bootmenu_seal_disabled(void);
bool qweovo_bootmenu_status_inode(const struct inode *inode);
bool qweovo_bootmenu_api_inode(const struct inode *inode);
bool qweovo_bootmenu_manager_inode(const struct inode *inode);
bool selinux_meow_manager_cache_sid(void);
bool qweovo_sukisu_next_supported(void);
#else
static inline bool qweovo_bootmenu_sukisu_enabled(void)
{
	return true;
}
static inline bool qweovo_bootmenu_meow_su_enabled(void)
{
	return false;
}
static inline bool qweovo_meow_su_grants_inode(const struct inode *inode)
{
	return false;
}
static inline bool qweovo_meow_su_uid_allowed(u32 uid)
{
	return false;
}
static inline bool qweovo_bootmenu_custom_enabled(void)
{
	return true;
}
static inline bool qweovo_bootmenu_status_spoofer_enabled(void)
{
	return false;
}
static inline bool qweovo_bootmenu_tee_enabled(void)
{
	return false;
}
static inline bool qweovo_bootmenu_module_lock_requested(void)
{
	return false;
}
static inline bool qweovo_bootmenu_selinux_locked(void) { return false; }
static inline bool qweovo_bootmenu_selinux_lock_requested(void) { return false; }
static inline bool qweovo_bootmenu_selinux_lock_writer(const struct inode *inode) { return false; }
static inline bool qweovo_bootmenu_audit_enabled(void) { return false; }
static inline bool qweovo_security_audit_available(void) { return false; }
static inline bool qweovo_security_audit_inode(const struct inode *inode) { return false; }
static inline void qweovo_security_audit(const char *event, int result) {}
static inline void qweovo_security_pipe_merge_blocked(void) {}
static inline bool qweovo_bootmenu_committed(void)
{
	return true;
}
static inline int qweovo_bootmenu_install_fd(void)
{
	return -EOPNOTSUPP;
}
static inline void qweovo_bootmenu_seal_disabled(void)
{
}
static inline bool qweovo_bootmenu_status_inode(const struct inode *inode)
{
	return false;
}
static inline bool qweovo_bootmenu_api_inode(const struct inode *inode)
{
	return false;
}
static inline bool qweovo_bootmenu_manager_inode(const struct inode *inode)
{
	return false;
}
static inline bool selinux_meow_manager_cache_sid(void)
{
	return false;
}
static inline bool qweovo_sukisu_next_supported(void)
{
	return false;
}
#endif

static inline struct proc_dir_entry *qweovo_manager_proc_create(const char *name,
		umode_t mode, const struct proc_ops *ops, void *marker)
{
	struct proc_dir_entry *entry;

#ifdef CONFIG_QWEOVO_BOOT_MENU
	entry = proc_create_data(name, mode & 0600, NULL, ops, marker);
	if (entry)
		proc_set_user(entry, KUIDT_INIT(MEOW_MANAGER_UID), GLOBAL_ROOT_GID);
#else
	entry = proc_create_data(name, mode, NULL, ops, marker);
#endif
	return entry;
}

#endif
