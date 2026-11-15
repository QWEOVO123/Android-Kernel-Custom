/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_QWEOVO_RAMDUMP_H
#define _LINUX_QWEOVO_RAMDUMP_H

#include <linux/types.h>

struct inode;

#ifdef CONFIG_QWEOVO_QCOM_PANIC_RAMDUMP
void qweovo_panic_prepare(void);
bool qweovo_public_debug_inode(const struct inode *inode);
bool qweovo_kernel_info_inode(const struct inode *inode);
#else
static inline void qweovo_panic_prepare(void)
{
}

static inline bool qweovo_public_debug_inode(const struct inode *inode)
{
	return false;
}
static inline bool qweovo_kernel_info_inode(const struct inode *inode)
{
	return false;
}
#endif

#endif /* _LINUX_QWEOVO_RAMDUMP_H */
