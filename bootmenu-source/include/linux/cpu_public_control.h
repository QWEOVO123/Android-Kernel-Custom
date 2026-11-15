/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CPU_PUBLIC_CONTROL_H
#define _LINUX_CPU_PUBLIC_CONTROL_H

#include <linux/types.h>

struct inode;

#ifdef CONFIG_CPU_PUBLIC_CONTROL
bool cpu_public_control_inode(const struct inode *inode);
#else
static inline bool cpu_public_control_inode(const struct inode *inode)
{
	return false;
}
#endif

#endif /* _LINUX_CPU_PUBLIC_CONTROL_H */
