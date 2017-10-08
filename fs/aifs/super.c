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

#include "aifs.h"

/*
 * The inode cache is used with alloc_inode for both our inode info and the
 * vfs inode.
 */
static struct kmem_cache *aifs_inode_cachep;

/* final actions when unmounting a file system */
static void aifs_put_super(struct super_block *sb)
{
	struct aifs_sb_info *spd;
	struct super_block *s;

	spd = AIFS_SB(sb);
	if (!spd)
		return;

	/* decrement lower super references */
	s = aifs_lower_super(sb);
	aifs_set_lower_super(sb, NULL);
	atomic_dec(&s->s_active);

	kfree(spd);
	sb->s_fs_info = NULL;
}

static int aifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int err;
	struct path lower_path;

	aifs_get_lower_path(dentry, &lower_path);
	err = vfs_statfs(&lower_path, buf);
	aifs_put_lower_path(dentry, &lower_path);

	/* set return buf to our f/s to avoid confusing user-level utils */
	buf->f_type = AIFS_SUPER_MAGIC;

	return err;
}

/*
 * @flags: numeric mount options
 * @options: mount options string
 */
static int aifs_remount_fs(struct super_block *sb, int *flags, char *options)
{
	int err = 0;

	/*
	 * The VFS will take care of "ro" and "rw" flags among others.  We
	 * can safely accept a few flags (RDONLY, MANDLOCK), and honor
	 * SILENT, but anything else left over is an error.
	 */
	if ((*flags & ~(MS_RDONLY | MS_MANDLOCK | MS_SILENT)) != 0) {
		printk(KERN_ERR
		       "aifs: remount flags 0x%x unsupported\n", *flags);
		err = -EINVAL;
	}

	return err;
}

/*
 * Called by iput() when the inode reference count reached zero
 * and the inode is not hashed anywhere.  Used to clear anything
 * that needs to be, before the inode is completely destroyed and put
 * on the inode free list.
 */
static void aifs_evict_inode(struct inode *inode)
{
	struct inode *lower_inode;

	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	/*
	 * Decrement a reference to a lower_inode, which was incremented
	 * by our read_inode when it was created initially.
	 */
	lower_inode = aifs_lower_inode(inode);
	aifs_set_lower_inode(inode, NULL);
	iput(lower_inode);
}

static struct inode *aifs_alloc_inode(struct super_block *sb)
{
	struct aifs_inode_info *i;

	i = kmem_cache_alloc(aifs_inode_cachep, GFP_KERNEL);
	if (!i)
		return NULL;

	/* memset everything up to the inode to 0 */
	memset(i, 0, offsetof(struct aifs_inode_info, vfs_inode));

	i->vfs_inode.i_version = 1;
	return &i->vfs_inode;
}

static void aifs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(aifs_inode_cachep, AIFS_I(inode));
}

/* aifs inode cache constructor */
static void init_once(void *obj)
{
	struct aifs_inode_info *i = obj;

	inode_init_once(&i->vfs_inode);
}

int aifs_init_inode_cache(void)
{
	int err = 0;

	aifs_inode_cachep =
		kmem_cache_create("aifs_inode_cache",
				  sizeof(struct aifs_inode_info), 0,
				  SLAB_RECLAIM_ACCOUNT, init_once);
	if (!aifs_inode_cachep)
		err = -ENOMEM;
	return err;
}

/* aifs inode cache destructor */
void aifs_destroy_inode_cache(void)
{
	if (aifs_inode_cachep)
		kmem_cache_destroy(aifs_inode_cachep);
}

/*
 * Used only in nfs, to kill any pending RPC tasks, so that subsequent
 * code can actually succeed and won't leave tasks that need handling.
 */
static void aifs_umount_begin(struct super_block *sb)
{
	struct super_block *lower_sb;

	lower_sb = aifs_lower_super(sb);
	if (lower_sb && lower_sb->s_op && lower_sb->s_op->umount_begin)
		lower_sb->s_op->umount_begin(lower_sb);
}

int aifs_show_options(struct seq_file *m, struct dentry *root)
{
#if 0
	struct super_block *lower_sb;
	char *buf;
	lower_sb = aifs_lower_super(root->d_sb);
	buf = (char *)__get_free_page(GFP_ATOMIC);
	if(buf) {
		char * path = dentry_path_raw(lower_sb->s_root, buf, PAGE_SIZE);
		seq_show_option(m, "lower", path);
		free_page((unsigned long)buf);
	}
#endif
	return 0;
}

const struct super_operations aifs_sops = {
	.put_super	= aifs_put_super,
	.statfs		= aifs_statfs,
	.remount_fs	= aifs_remount_fs,
	.evict_inode	= aifs_evict_inode,
	.umount_begin	= aifs_umount_begin,
	.show_options	= aifs_show_options,
	.alloc_inode	= aifs_alloc_inode,
	.destroy_inode	= aifs_destroy_inode,
	.drop_inode	= generic_delete_inode,
};

/* NFS support */

static struct inode *aifs_nfs_get_inode(struct super_block *sb, u64 ino,
					  u32 generation)
{
	struct super_block *lower_sb;
	struct inode *inode;
	struct inode *lower_inode;

	lower_sb = aifs_lower_super(sb);
	lower_inode = ilookup(lower_sb, ino);
	inode = aifs_iget(sb, lower_inode);
	return inode;
}

static struct dentry *aifs_fh_to_dentry(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    aifs_nfs_get_inode);
}

static struct dentry *aifs_fh_to_parent(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    aifs_nfs_get_inode);
}

/*
 * all other funcs are default as defined in exportfs/expfs.c
 */

const struct export_operations aifs_export_ops = {
	.fh_to_dentry	   = aifs_fh_to_dentry,
	.fh_to_parent	   = aifs_fh_to_parent
};
