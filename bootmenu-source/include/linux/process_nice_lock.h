/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PROCESS_NICE_LOCK_H
#define _LINUX_PROCESS_NICE_LOCK_H

#include <linux/types.h>

struct inode;
struct task_struct;

#define PROCESS_NICE_LOCK_MAX_RULES 256

#ifdef CONFIG_PROCESS_UID_NICE_LOCK
bool process_uid_nice_value(const struct task_struct *task, int *nice);
void process_uid_nice_apply(struct task_struct *task);
bool process_nice_lock_inode(const struct inode *inode);
bool process_nice_lock_supported(void);
unsigned int process_nice_lock_count(void);
#else
static inline bool process_uid_nice_value(const struct task_struct *task, int *nice)
{
	return false;
}
static inline void process_uid_nice_apply(struct task_struct *task) {}
static inline bool process_nice_lock_inode(const struct inode *inode)
{
	return false;
}
static inline bool process_nice_lock_supported(void)
{
	return false;
}
static inline unsigned int process_nice_lock_count(void)
{
	return 0;
}
#endif
#endif /* _LINUX_PROCESS_NICE_LOCK_H */
