/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_TTL_PUBLIC_CONTROL_H
#define _LINUX_TTL_PUBLIC_CONTROL_H

#include <linux/types.h>

struct inode;

#ifdef CONFIG_IP_FORWARD_TTL_PUBLIC_CONTROL
bool ttl_public_control_inode(const struct inode *inode);
#else
static inline bool ttl_public_control_inode(const struct inode *inode)
{
	return false;
}
#endif

#endif /* _LINUX_TTL_PUBLIC_CONTROL_H */
