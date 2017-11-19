#ifndef _AIFS_DIR_H
#define _AIFS_DIR_H

#include <linux/kernel.h>
#include <linux/uuid.h>

static inline int aifs_do_rmdir(struct inode *dir, struct dentry *dentry)
{
	int err = vfs_rmdir(dir, dentry);
	pr_debug("rmdir(%pd2) = %i\n", dentry, err);
	return err;
}

static inline int aifs_do_unlink(struct inode *dir, struct dentry *dentry)
{
	int err = vfs_unlink(dir, dentry, NULL);
	pr_debug("unlink(%pd2) = %i\n", dentry, err);
	return err;
}

static inline int aifs_do_link(struct dentry *old_dentry, struct inode *dir,
			      struct dentry *new_dentry, bool debug)
{
	int err = vfs_link(old_dentry, dir, new_dentry, NULL);
	if (debug) {
		pr_debug("link(%pd2, %pd2) = %i\n",
			 old_dentry, new_dentry, err);
	}
	return err;
}

static inline int aifs_do_create(struct inode *dir, struct dentry *dentry,
			     umode_t mode, bool debug)
{
	int err = vfs_create(dir, dentry, mode, true);
	if (debug)
		pr_debug("create(%pd2, 0%o) = %i\n", dentry, mode, err);
	return err;
}

static inline int aifs_do_mkdir(struct inode *dir, struct dentry *dentry,
			       umode_t mode, bool debug)
{
	int err = vfs_mkdir(dir, dentry, mode);
	if (debug)
		pr_debug("mkdir(%pd2, 0%o) = %i\n", dentry, mode, err);
	return err;
}

static inline int aifs_do_mknod(struct inode *dir, struct dentry *dentry,
			       umode_t mode, dev_t dev, bool debug)
{
	int err = vfs_mknod(dir, dentry, mode, dev);
	if (debug) {
		pr_debug("mknod(%pd2, 0%o, 0%o) = %i\n",
			 dentry, mode, dev, err);
	}
	return err;
}

static inline int aifs_do_symlink(struct inode *dir, struct dentry *dentry,
				 const char *oldname, bool debug)
{
	int err = vfs_symlink(dir, dentry, oldname);
	if (debug)
		pr_debug("symlink(\"%s\", %pd2) = %i\n", oldname, dentry, err);
	return err;
}

static inline int aifs_do_setxattr(struct dentry *dentry, const char *name,
				  const void *value, size_t size, int flags)
{
	int err = vfs_setxattr(dentry, name, value, size, flags);
	pr_debug("setxattr(%pd2, \"%s\", \"%*s\", 0x%x) = %i\n",
		 dentry, name, (int) size, (char *) value, flags, err);
	return err;
}

static inline int aifs_do_removexattr(struct dentry *dentry, const char *name)
{
	int err = vfs_removexattr(dentry, name);
	pr_debug("removexattr(%pd2, \"%s\") = %i\n", dentry, name, err);
	return err;
}

static inline int aifs_do_rename(struct inode *olddir, struct dentry *olddentry,
				struct inode *newdir, struct dentry *newdentry,
				unsigned int flags)
{
	int err;

	pr_debug("rename(%pd2, %pd2, 0x%x)\n",
		 olddentry, newdentry, flags);

	err = vfs_rename(olddir, olddentry, newdir, newdentry, NULL, flags);

	if (err) {
		pr_debug("...rename(%pd2, %pd2, ...) = %i\n",
			 olddentry, newdentry, err);
	}
	return err;
}

static inline int aifs_do_whiteout(struct inode *dir, struct dentry *dentry)
{
	int err = vfs_whiteout(dir, dentry);
	pr_debug("whiteout(%pd2) = %i\n", dentry, err);
	return err;
}

static inline struct dentry *aifs_do_tmpfile(struct dentry *dentry, umode_t mode)
{
	struct dentry *ret = vfs_tmpfile(dentry, mode, 0);
	int err = IS_ERR(ret) ? PTR_ERR(ret) : 0;

	pr_debug("tmpfile(%pd2, 0%o) = %i\n", dentry, mode, err);
	return ret;
}

static inline void aifs_copyattr(struct inode *from, struct inode *to)
{
	to->i_uid = from->i_uid;
	to->i_gid = from->i_gid;
	to->i_mode = from->i_mode;
	to->i_atime = from->i_atime;
	to->i_mtime = from->i_mtime;
	to->i_ctime = from->i_ctime;
}

int aifs_create_real(struct inode *dir, struct dentry *newdentry,
		    struct cattr *attr,
		    struct dentry *hardlink, bool debug);

int aifs_cleanup(struct inode *dir, struct dentry *dentry);

#endif /* _AIFS_DIR_H */
