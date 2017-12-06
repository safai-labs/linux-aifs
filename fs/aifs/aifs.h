/*
 * Copyright (c) 1998-2017 Erez Zadok
 * Copyright (c) 2009	   Shrikar Archak
 * Copyright (c) 2003-2017 Stony Brook University
 * Copyright (c) 2003-2017 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _AIFS_H_
#define _AIFS_H_

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <linux/fs_stack.h>
#include <linux/magic.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/xattr.h>
#include <linux/exportfs.h>
#include <linux/uio.h>
#include "ovl_entry.h"
#include "overlayfs.h"
#include "dir.h"

/* the file system name */
#define AIFS_NAME "aifs"

/* aifs root inode number */
#define AIFS_ROOT_INO     1

/* useful for tracking code reachability */
#define UDBG printk(KERN_DEFAULT "DBG:%s:%s:%d\n", __FILE__, __func__, __LINE__)

/* operations vectors defined in specific files */
extern const struct file_operations aifs_main_fops;
extern const struct file_operations aifs_dir_fops;
extern const struct inode_operations aifs_main_iops;
extern const struct inode_operations aifs_dir_iops;
extern const struct inode_operations aifs_symlink_iops;
extern const struct super_operations aifs_sops;
extern const struct dentry_operations aifs_dops;
extern const struct address_space_operations aifs_aops, aifs_dummy_aops;
extern const struct vm_operations_struct aifs_vm_ops;
extern const struct export_operations aifs_export_ops;
extern const struct xattr_handler *aifs_xattr_handlers[];

extern int aifs_init_inode_cache(void);
extern void aifs_destroy_inode_cache(void);
extern int aifs_init_dentry_cache(void);
extern void aifs_destroy_dentry_cache(void);
extern int new_dentry_private_data(struct dentry *dentry);
extern void free_dentry_private_data(struct dentry *dentry);
extern struct dentry *aifs_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags);
extern struct inode *aifs_iget(struct super_block *sb,
				 struct inode *lower_inode);
extern int aifs_interpose(struct dentry *dentry, struct super_block *sb,
			    struct path *lower_path);

/* super block functions */


/* file private data */
struct aifs_file_info {
	struct file *lower_file;
	const struct vm_operations_struct *lower_vm_ops;
#if defined(AIFS_DEBUG)
	char fullpath[256]; /* XXX: remove this later */
#endif
};

/* aifs inode data in memory */
struct aifs_inode_info {
	struct inode *lower_inode;
	struct inode vfs_inode;
};

/* aifs dentry data in memory */
struct aifs_dentry_info {
	spinlock_t lock;	/* protects lower_path */
	struct path lower_path;
};

/* aifs super-block data in memory */
#define AIFS_WORK_BASEDIR_NAME "._aifs"
#define AIFS_WORK_DATADIR_NAME "data"
#define AIFS_WORK_METADIR_NAME "meta"

struct aifs_sb_info {
	struct super_block *lower_sb;
	struct _aifs_work {
		struct path    parent;
		struct dentry *basedir;
		struct dentry *datadir;
		struct dentry *metadir;
	} work;
};


/*
 * inode to private data
 *
 * Since we use containers and the struct inode is _inside_ the
 * aifs_inode_info structure, AIFS_I will always (given a non-NULL
 * inode pointer), return a valid non-NULL pointer.
 */
static inline struct aifs_inode_info *AIFS_I(const struct inode *inode)
{
	return container_of(inode, struct aifs_inode_info, vfs_inode);
}

/* dentry to private data */
#define AIFS_D(dent) ((struct aifs_dentry_info *)(dent)->d_fsdata)

/* superblock to private data */
#define AIFS_SB(super) ((struct aifs_sb_info *)(super)->s_fs_info)

/* file to private Data */
#define AIFS_F(file) ((struct aifs_file_info *)((file)->private_data))

/* dentry to lower dentry */

static inline struct dentry *aifs_lower_dentry(const struct dentry *dentry)
{
	return AIFS_D(dentry)->lower_path.dentry;
}

/* file to lower file */
static inline struct file *aifs_lower_file(const struct file *f)
{
	return AIFS_F(f)->lower_file;
}

static inline void aifs_set_lower_file(struct file *f, struct file *val)
{
	AIFS_F(f)->lower_file = val;
}

/* inode to lower inode. */
static inline struct inode *aifs_lower_inode(const struct inode *i)
{
	return AIFS_I(i)->lower_inode;
}

static inline void aifs_set_lower_inode(struct inode *i, struct inode *val)
{
	AIFS_I(i)->lower_inode = val;
}

/* superblock to lower superblock */
static inline struct super_block *aifs_lower_super(
	const struct super_block *sb)
{
	return AIFS_SB(sb)->lower_sb;
}


/* path based (dentry/mnt) macros */
static inline void pathcpy(struct path *dst, const struct path *src)
{
	dst->dentry = src->dentry;
	dst->mnt = src->mnt;
}
/* Returns struct path.  Caller must path_put it. */
static inline void aifs_get_lower_path(const struct dentry *dent,
					 struct path *lower_path)
{
	spin_lock(&AIFS_D(dent)->lock);
	pathcpy(lower_path, &AIFS_D(dent)->lower_path);
	path_get(lower_path);
	spin_unlock(&AIFS_D(dent)->lock);
	return;
}
static inline void aifs_put_lower_path(const struct dentry *dent,
					 struct path *lower_path)
{
	path_put(lower_path);
	return;
}
static inline void aifs_set_lower_path(const struct dentry *dent,
					 struct path *lower_path)
{
	spin_lock(&AIFS_D(dent)->lock);
	pathcpy(&AIFS_D(dent)->lower_path, lower_path);
	spin_unlock(&AIFS_D(dent)->lock);
	return;
}
static inline void aifs_reset_lower_path(const struct dentry *dent)
{
	spin_lock(&AIFS_D(dent)->lock);
	AIFS_D(dent)->lower_path.dentry = NULL;
	AIFS_D(dent)->lower_path.mnt = NULL;
	spin_unlock(&AIFS_D(dent)->lock);
	return;
}
static inline void aifs_put_reset_lower_path(const struct dentry *dent)
{
	struct path lower_path;
	spin_lock(&AIFS_D(dent)->lock);
	pathcpy(&lower_path, &AIFS_D(dent)->lower_path);
	AIFS_D(dent)->lower_path.dentry = NULL;
	AIFS_D(dent)->lower_path.mnt = NULL;
	spin_unlock(&AIFS_D(dent)->lock);
	path_put(&lower_path);
	return;
}

static inline void aifs_reset_super(const struct super_block *sb) 
{
	struct aifs_sb_info *aifs = AIFS_SB(sb);
	memset(aifs, 0, sizeof(*aifs));
}

/* locking helpers */
static inline struct dentry *lock_parent(struct dentry *dentry)
{
	struct dentry *dir = dget_parent(dentry);
	inode_lock_nested(d_inode(dir), I_MUTEX_PARENT);
	return dir;
}

static inline void unlock_dir(struct dentry *dir)
{
	inode_unlock(d_inode(dir));
	dput(dir);
}


static inline struct ovl_fs * aifs_ovl_fs(struct aifs_sb_info *aifs)
{
	return (struct ovl_fs *)(aifs->lower_sb->s_fs_info);
}


#endif	/* not _AIFS_H_ */
