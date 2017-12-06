/*
 * Copyright (c) 2017 Ahmed Masud
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
/* TODO: this needs to be replaced up by lookup_one */
extern int vfs_path_lookup(struct dentry *, struct vfsmount *,
	const char *, unsigned int, struct path *);

/* The dentry cache is just so we have properly sized dentries */
static struct kmem_cache *aifs_dentry_cachep;

int aifs_init_dentry_cache(void)
{
	aifs_dentry_cachep =
		kmem_cache_create("aifs_dentry",
				  sizeof(struct aifs_dentry_info),
				  0, SLAB_RECLAIM_ACCOUNT, NULL);

	return aifs_dentry_cachep ? 0 : -ENOMEM;
}

void aifs_destroy_dentry_cache(void)
{
	if (aifs_dentry_cachep)
		kmem_cache_destroy(aifs_dentry_cachep);
}

void free_dentry_private_data(struct dentry *dentry)
{
	if (!dentry || !dentry->d_fsdata)
		return;
	kmem_cache_free(aifs_dentry_cachep, dentry->d_fsdata);
	dentry->d_fsdata = NULL;
}

/* allocate new dentry private data */
int new_dentry_private_data(struct dentry *dentry)
{
	struct aifs_dentry_info *info = AIFS_D(dentry);

	/* use zalloc to init dentry_info.lower_path */
	info = kmem_cache_zalloc(aifs_dentry_cachep, GFP_ATOMIC);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	dentry->d_fsdata = info;

	return 0;
}

static int aifs_inode_test(struct inode *inode, void *candidate_lower_inode)
{
	struct inode *current_lower_inode = aifs_lower_inode(inode);
	if (current_lower_inode == (struct inode *)candidate_lower_inode)
		return 1; /* found a match */
	else
		return 0; /* no match */
}

static int aifs_inode_set(struct inode *inode, void *lower_inode)
{
	/* we do actual inode initialization in aifs_iget */
	return 0;
}

struct inode *aifs_iget(struct super_block *sb, struct inode *lower_inode)
{
	struct aifs_inode_info *info;
	struct inode *inode; /* the new inode to return */

	if (!igrab(lower_inode))
		return ERR_PTR(-ESTALE);
	inode = iget5_locked(sb, /* our superblock */
			     /*
			      * hashval: we use inode number, but we can
			      * also use "(unsigned long)lower_inode"
			      * instead.
			      */
			     lower_inode->i_ino, /* hashval */
			     aifs_inode_test,	/* inode comparison function */
			     aifs_inode_set, /* inode init function */
			     lower_inode); /* data passed to test+set fxns */
	if (!inode) {
		iput(lower_inode);
		return ERR_PTR(-ENOMEM);
	}
	/* if found a cached inode, then just return it (after iput) */
	if (!(inode->i_state & I_NEW)) {
		iput(lower_inode);
		return inode;
	}

	/* initialize new inode */
	info = AIFS_I(inode);

	inode->i_ino = lower_inode->i_ino;
	aifs_set_lower_inode(inode, lower_inode);

	inode->i_version++;

	/* use different set of inode ops for symlinks & directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &aifs_dir_iops;
	else if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &aifs_symlink_iops;
	else
		inode->i_op = &aifs_main_iops;

	/* use different set of file ops for directories */
	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &aifs_dir_fops;
	else
		inode->i_fop = &aifs_main_fops;

	inode->i_mapping->a_ops = &aifs_aops;

	inode->i_atime.tv_sec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = 0;
	inode->i_mtime.tv_nsec = 0;
	inode->i_ctime.tv_sec = 0;
	inode->i_ctime.tv_nsec = 0;

	/* properly initialize special inodes */
	if (S_ISBLK(lower_inode->i_mode) || S_ISCHR(lower_inode->i_mode) ||
	    S_ISFIFO(lower_inode->i_mode) || S_ISSOCK(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				   lower_inode->i_rdev);

	/* all well, copy inode attributes */
	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);

	unlock_new_inode(inode);
	return inode;
}

/*
 * Helper interpose routine, called directly by ->lookup to handle
 * spliced dentries.
 */
static struct dentry *__aifs_interpose(struct dentry *dentry,
					 struct super_block *sb,
					 struct path *lower_path)
{
	struct inode *inode;
	struct inode *lower_inode;
	struct super_block *lower_sb;
	struct dentry *ret_dentry;

	lower_inode = d_inode(lower_path->dentry);
	lower_sb = aifs_lower_super(sb);

	/* check that the lower file system didn't cross a mount point */
	if (lower_inode->i_sb != lower_sb) {
		ret_dentry = ERR_PTR(-EXDEV);
		goto out;
	}

	/*
	 * We allocate our new inode below by calling aifs_iget,
	 * which will initialize some of the new inode's fields
	 */

	/* inherit lower inode number for aifs's inode */
	inode = aifs_iget(sb, lower_inode);
	if (IS_ERR(inode)) {
		ret_dentry = ERR_PTR(PTR_ERR(inode));
		goto out;
	}

	ret_dentry = d_splice_alias(inode, dentry);

out:
	return ret_dentry;
}

/*
 * Connect a aifs inode dentry/inode with several lower ones.  This is
 * the classic stackable file system "vnode interposition" action.
 *
 * @dentry: aifs's dentry which interposes on lower one
 * @sb: aifs's super_block
 * @lower_path: the lower path (caller does path_get/put)
 */
int aifs_interpose(struct dentry *dentry, struct super_block *sb,
		     struct path *lower_path)
{
	struct dentry *ret_dentry;

	ret_dentry = __aifs_interpose(dentry, sb, lower_path);
	return PTR_ERR(ret_dentry);
}

/*
 * Main driver function for aifs's lookup.
 *
 * Returns: NULL (ok), ERR_PTR if an error occurred.
 * Fills in lower_parent_path with <dentry,mnt> on success.
 */


//
// THIS REQUIRES MAJOR CLEANUP and REWRITE 
//
static struct dentry *__aifs_lookup(struct dentry *dentry,
				      unsigned int flags,
				      struct path *lower_parent_path)
{
	int err = 0;
	struct vfsmount *lower_dir_mnt = NULL;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry;
	struct path lower_path;
	struct qstr this;
	struct dentry *ret_dentry = NULL;

	/* must initialize dentry operations */
	d_set_d_op(dentry, &aifs_dops);

	if (IS_ROOT(dentry))
		goto out;

	// name = dentry->d_name.name;

	/* now start the actual lookup procedure */
	lower_dir_dentry = lower_parent_path->dentry;
	lower_dir_mnt = lower_parent_path->mnt;

	/* Use vfs_path_lookup to check if the dentry exists or not */
	err = vfs_path_lookup(lower_dir_dentry, lower_dir_mnt, dentry->d_name.name, 0, &lower_path);
	
#if 0
	lower_dentry = lookup_one_len_unlocked(dentry->d_name.name, lower_parent_path->dentry, strlen(dentry->d_name.name));
	err = PTR_ERR(lower_dentry);
#endif
	/* no error: handle positive dentries */
	if (!err) {
		// lower_path.dentry = lower_dentry;
		// lower_path.mnt = mntget(lower_parent_path->mnt);
		aifs_set_lower_path(dentry, &lower_path);
		ret_dentry =
			__aifs_interpose(dentry, dentry->d_sb, &lower_path);
		if (IS_ERR(ret_dentry)) {
			err = PTR_ERR(ret_dentry);
			 /* path_put underlying path on error */
			aifs_put_reset_lower_path(dentry);
		}
		goto out;
	}

	/*
	 * We don't consider ENOENT an error, and we want to return a
	 * negative dentry.
	 */
	if (err && err != -ENOENT)
		goto out;

	/* instatiate a new negative dentry */
	this.name = dentry->d_name.name;
	this.len = dentry->d_name.len;
	this.hash = full_name_hash(lower_dir_dentry, this.name, this.len);
	lower_dentry = d_lookup(lower_dir_dentry, &this);
	if (lower_dentry)
		goto setup_lower;

	lower_dentry = d_alloc(lower_dir_dentry, &this);
	if (!lower_dentry) {
		err = -ENOMEM;
		goto out;
	}
	d_add(lower_dentry, NULL); /* instantiate and hash */

setup_lower:
	lower_path.dentry = lower_dentry;
	lower_path.mnt = mntget(lower_dir_mnt); /* may be crashing here? */
	aifs_set_lower_path(dentry, &lower_path);

	/*
	 * If the intent is to create a file, then don't return an error, so
	 * the VFS will continue the process of making this negative dentry
	 * into a positive one.
	 */
	if (err == -ENOENT || (flags & (LOOKUP_CREATE|LOOKUP_RENAME_TARGET)))
		err = 0;

out:
	if (err)
		return ERR_PTR(err);
	return ret_dentry;
}

struct dentry *aifs_lookup(struct inode *dir, struct dentry *dentry,
			     unsigned int flags)
{
	int err;
	struct dentry *ret, *parent;
	struct path lower_parent_path;

	parent = dget_parent(dentry);

	aifs_get_lower_path(parent, &lower_parent_path);

	/* allocate dentry private data.  We free it in ->d_release */
	err = new_dentry_private_data(dentry);
	if (err) {
		ret = ERR_PTR(err);
		goto out;
	}
	ret = __aifs_lookup(dentry, flags, &lower_parent_path);
	if (IS_ERR(ret))
		goto out;
	if (ret)
		dentry = ret;
	if (d_inode(dentry))
		fsstack_copy_attr_times(d_inode(dentry),
					aifs_lower_inode(d_inode(dentry)));
	/* update parent directory's atime */
	fsstack_copy_attr_atime(d_inode(parent),
				aifs_lower_inode(d_inode(parent)));

out:
	aifs_put_lower_path(parent, &lower_parent_path);
	dput(parent);
	return ret;
}
