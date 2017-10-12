#ifndef __AIFS_OVL_ENTRY_H
#define __AIFS_OVL_ENTRY_H
#include "../overlayfs/ovl_entry.h"
#include "aifs.h"

static inline struct ovl_fs * aifs_ovl_fs(struct super_block *sb)
{
	return (struct ovl_fs *)aifs_lower_super(sb)->s_fs_info;
}

#endif /* __AIFS_OVL_ENTRY_H */
