/*
 * Built using the excellent aifs (aifs.filesystems.org)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "aifs.h"
#include <linux/module.h>
#include <linux/fs.h>

/* right now no clean up is performed */
static void aifs_cleanup_workdir(struct inode *dir, struct vfsmount *vfs,
		struct dentry *dentry, int level)
{
	return;
}

/* 
 * create working directory
 */

static struct dentry *aifs_create_workdir(struct aifs_sb_info *aifs, const char *name, bool persist)
{
	struct dentry *dentry = aifs->work.basedir;
	struct inode *dir = dentry->d_inode;
	struct ovl_fs *ofs = aifs_ovl_fs(aifs);
	struct vfsmount *mnt = ofs->upper_mnt;
	struct dentry *workdir;
	int err;
	bool locked, retried;

	err = mnt_want_write(mnt);
	if (err)
		goto out_err;

	inode_lock_nested(dir, I_MUTEX_PARENT);
	locked = true;

 retry:
	workdir = lookup_one_len(name, dentry, strlen(name));

	if(!IS_ERR(workdir)) {
		struct iattr attr = {
			.ia_valid = ATTR_MODE, 
			.ia_mode = S_IFDIR | 0,
		};

		if(workdir->d_inode) {
			err = -EEXIST;
			/* TODO: add code for cleaning up any transient directories here */
			if(persist) 
				goto out_unlock;

			retried = true;
			aifs_cleanup_workdir(dir, mnt, workdir, 0);
			dput(workdir);
			goto retry;
		}

		err = aifs_create_real(dir, workdir, &(struct cattr) { .mode = S_IFDIR | 0}, NULL, true);

		if(err) 
			goto out_dput;

                err = vfs_removexattr(workdir, XATTR_NAME_POSIX_ACL_DEFAULT);
                if (err && err != -ENODATA && err != -EOPNOTSUPP)
                        goto out_dput;

                err = vfs_removexattr(workdir, XATTR_NAME_POSIX_ACL_ACCESS);
                if (err && err != -ENODATA && err != -EOPNOTSUPP)
                        goto out_dput;

                /* Clear any inherited mode bits */
                inode_lock(workdir->d_inode);
                err = notify_change(workdir, &attr, NULL);
                inode_unlock(workdir->d_inode);
                if (err)
                        goto out_dput;
	} else {
		err = PTR_ERR(workdir);
		goto out_err;
	}
 out_unlock:
	mnt_drop_write(mnt);
	if(locked)
		inode_unlock(dir);
	return workdir;

 out_dput:
	dput(workdir);

 out_err:
	workdir = NULL;
	pr_err
	    ("aifs: failed to create management directories %s/%s (errno: %i); refusing to continue\n",
	     ofs->config.workdir, name, -err);
	goto out_unlock;
}

/*
 * There is no need to lock the aifs_super_info's rwsem as there is no
 * way anyone can have a reference to the superblock at this point in time.
 */

static inline int aifs_set_lower_super(struct super_block *sb,
				       struct super_block *val)
{
	struct path workpath = { };
	int err = -EIO;
	struct ovl_fs *ofs;
        if ( !val ) {
		return err;
	}
	ofs = val->s_fs_info;
	if (!ofs->config.workdir)  {
		pr_err("aifs: read-only overlayfs not supported");
		return -EINVAL;
	}

	err = kern_path(ofs->config.workdir, LOOKUP_FOLLOW, &workpath);
	if (err) {
		pr_err("aifs: found overlayfs with workdir, but it disappeared");
		return -EIO;
	}
	err = 0;
	AIFS_SB(sb)->lower_sb = val;
	AIFS_SB(sb)->work.basedir = aifs_create_workdir(AIFS_SB(sb), AIFS_WORK_BASEDIR_NAME, true);
	AIFS_SB(sb)->work.datadir = aifs_create_workdir(AIFS_SB(sb), AIFS_WORK_BASEDIR_NAME "/" AIFS_WORK_DATADIR_NAME, true);
	AIFS_SB(sb)->work.metadir = aifs_create_workdir(AIFS_SB(sb), AIFS_WORK_BASEDIR_NAME "/" AIFS_WORK_METADIR_NAME, true);
	return err;
}

// TODO: 1) Should really look at overlayfs magic number rather than name
//       2) Add check to ensure that overlayfs has an upper fs
//       3a) For now ensure that upperfs of overlayfs is actually btrfs
//       3b) Make sure that upperfs supports reflink
static int aifs_unsupported_lower(struct super_block *lower_sb)
{
	if (strcmp(lower_sb->s_type->name, "overlay")) {
		pr_err
		    ("AiFS only works with overlayfs as the lower file-system\n");
		return -1;
	}
	return 0;
}

static int aifs_fill_super(struct super_block *sb, void *raw_data, int silent)
{
	int err = 0;
	struct super_block *lower_sb;
	struct ovl_fs *ofs;
	struct path lower_path;
	char *dev_name = (char *)raw_data;
	struct inode *inode;

	if (!dev_name) {
		printk(KERN_ERR
		       "aifs: read_super: missing dev_name argument\n");
		err = -EINVAL;
		goto out;
	}

	/* parse lower path */
	err = kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&lower_path);
	if (err) {
		printk(KERN_ERR "aifs: error accessing "
		       "lower directory '%s'\n", dev_name);
		goto out;
	}

	/* allocate superblock private data */
	sb->s_fs_info = kzalloc(sizeof(struct aifs_sb_info), GFP_KERNEL);
	if (!AIFS_SB(sb)) {
		printk(KERN_CRIT "aifs: read_super: out of memory\n");
		err = -ENOMEM;
		goto out_free;
	}

	/* set the lower superblock field of upper superblock */
	lower_sb = lower_path.dentry->d_sb;
	atomic_inc(&lower_sb->s_active);

	if (aifs_unsupported_lower(lower_sb)) {
		err = -EIO;
		goto out_sput;
	}


	err = aifs_set_lower_super(sb, lower_sb);
	if(err) 
		goto out_sput;

	ofs = lower_sb->s_fs_info;
	pr_info
	    ("AiFS [mounted on overlayfs with upper=%s, lower=%s, workdir=%s]",
	     ofs->config.upperdir, ofs->config.lowerdir, ofs->config.workdir);

	/* inherit maxbytes from lower file system */
	sb->s_maxbytes = lower_sb->s_maxbytes;

	/*
	 * Our c/m/atime granularity is 1 ns because we may stack on file
	 * systems whose granularity is as good.
	 */
	sb->s_time_gran = 1;

	sb->s_op = &aifs_sops;
	sb->s_xattr = aifs_xattr_handlers;

	sb->s_export_op = &aifs_export_ops;	/* adding NFS support */

	/* get a new inode and allocate our root dentry */
	inode = aifs_iget(sb, d_inode(lower_path.dentry));
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_sput;
	}
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_iput;
	}
	d_set_d_op(sb->s_root, &aifs_dops);

	/* link the upper and lower dentries */
	sb->s_root->d_fsdata = NULL;
	err = new_dentry_private_data(sb->s_root);
	if (err)
		goto out_freeroot;

	/* if get here: cannot have error */

	/* set the lower dentries for s_root */
	aifs_set_lower_path(sb->s_root, &lower_path);

	/*
	 * No need to call interpose because we already have a positive
	 * dentry, which was instantiated by d_make_root.  Just need to
	 * d_rehash it.
	 */
	d_rehash(sb->s_root);
	if (!silent)
		printk(KERN_INFO
		       "AiFS: mounted on top of %s type %s\n",
		       dev_name, lower_sb->s_type->name);
	goto out;		/* all is well */

	/* no longer needed: free_dentry_private_data(sb->s_root); */
 out_freeroot:
	dput(sb->s_root);
 out_iput:
	iput(inode);
 out_sput:
	/* drop refs we took earlier */
	atomic_dec(&lower_sb->s_active);
	kfree(AIFS_SB(sb));
	sb->s_fs_info = NULL;
 out_free:
	path_put(&lower_path);

 out:
	return err;
}

struct dentry *aifs_mount(struct file_system_type *fs_type, int flags,
			  const char *dev_name, void *raw_data)
{
	void *lower_path_name = (void *)dev_name;

	return mount_nodev(fs_type, flags, lower_path_name, aifs_fill_super);
}

static struct file_system_type aifs_fs_type = {
	.owner = THIS_MODULE,
	.name = AIFS_NAME,
	.mount = aifs_mount,
	.kill_sb = generic_shutdown_super,
	.fs_flags = 0,
};

MODULE_ALIAS_FS(AIFS_NAME);

static int __init init_aifs_fs(void)
{
	int err;

	pr_info("Registering AiFS: saf.ai (aifs " AIFS_VERSION ")\n");

	err = aifs_init_inode_cache();
	if (err)
		goto out;
	err = aifs_init_dentry_cache();
	if (err)
		goto out;
	err = register_filesystem(&aifs_fs_type);
 out:
	if (err) {
		aifs_destroy_inode_cache();
		aifs_destroy_dentry_cache();
	}
	return err;
}

static void __exit exit_aifs_fs(void)
{
	aifs_destroy_inode_cache();
	aifs_destroy_dentry_cache();
	unregister_filesystem(&aifs_fs_type);
	pr_info("Completed aifs module unload\n");
}

MODULE_AUTHOR("Ahmed Masud <ahmed.masud@trustifier.com>"
	      " (http://trustifier.com/ahmed/)");
MODULE_DESCRIPTION("AiFS " AIFS_VERSION " (http://aifs.saf.ai/)");
MODULE_LICENSE("GPL");

module_init(init_aifs_fs);
module_exit(exit_aifs_fs);
