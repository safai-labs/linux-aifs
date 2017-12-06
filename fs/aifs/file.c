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

#if CONFIG_AIFS_LEGACY_READ
# error this is not supported
#endif

static ssize_t aifs_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	int err;


	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = aifs_lower_file(file);
	err = kernel_write(lower_file, buf, count, ppos);
	// err = vfs_write(lower_file, buf, count, ppos);
	/* update our inode times+sizes upon a successful lower write */
	if (err >= 0) {
		fsstack_copy_inode_size(d_inode(dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(dentry),
					file_inode(lower_file));
	}

	return err;
}

static int aifs_readdir(struct file *file, struct dir_context *ctx)
{
	int err;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = aifs_lower_file(file);
	err = iterate_dir(lower_file, ctx);
	file->f_pos = lower_file->f_pos;
	if (err >= 0)		/* copy the atime */
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));
	return err;
}

static long aifs_unlocked_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = aifs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

	/* some ioctls can change inode attributes (EXT2_IOC_SETFLAGS) */
	if (!err)
		fsstack_copy_attr_all(file_inode(file),
				      file_inode(lower_file));
out:
	return err;
}

#ifdef CONFIG_COMPAT
static long aifs_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = aifs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

out:
	return err;
}
#endif

static int aifs_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	bool willwrite;
	struct file *lower_file;
	const struct vm_operations_struct *saved_vm_ops = NULL;

	/* this might be deferred to mmap's writepage */
	willwrite = ((vma->vm_flags | VM_SHARED | VM_WRITE) == vma->vm_flags);

	/*
	 * File systems which do not implement ->writepage may use
	 * generic_file_readonly_mmap as their ->mmap op.  If you call
	 * generic_file_readonly_mmap with VM_WRITE, you'd get an -EINVAL.
	 * But we cannot call the lower ->mmap op, so we can't tell that
	 * writeable mappings won't work.  Therefore, our only choice is to
	 * check if the lower file system supports the ->writepage, and if
	 * not, return EINVAL (the same error that
	 * generic_file_readonly_mmap returns in that case).
	 */
	lower_file = aifs_lower_file(file);
	if (willwrite && !lower_file->f_mapping->a_ops->writepage) {
		err = -EINVAL;
		printk(KERN_ERR "aifs: lower file system does not "
		       "support writeable mmap\n");
		goto out;
	}

	/*
	 * find and save lower vm_ops.
	 *
	 * XXX: the VFS should have a cleaner way of finding the lower vm_ops
	 */
	if (!AIFS_F(file)->lower_vm_ops) {
		err = lower_file->f_op->mmap(lower_file, vma);
		if (err) {
			printk(KERN_ERR "aifs: lower mmap failed %d\n", err);
			goto out;
		}
		saved_vm_ops = vma->vm_ops; /* save: came from lower ->mmap */
	}

	/*
	 * Next 3 lines are all I need from generic_file_mmap.  I definitely
	 * don't want its test for ->readpage which returns -ENOEXEC.
	 */
	file_accessed(file);
	vma->vm_ops = &aifs_vm_ops;

	file->f_mapping->a_ops = &aifs_aops; /* set our aops */
	if (!AIFS_F(file)->lower_vm_ops) /* save for our ->fault */
		AIFS_F(file)->lower_vm_ops = saved_vm_ops;

out:
	return err;
}

#if defined(AIFS_DEBUG)
static char __tmp[4096];
#endif

static int aifs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct path lower_path;

	/* don't open unhashed/deleted files */
	if (d_unhashed(file->f_path.dentry)) {
		pr_err("aifs unhased dentry, returning -ENOENT for %p\n", file->f_path.dentry);
		err = -ENOENT;
		goto out_err;
	}

	pr_info("aifs, private data: %p\n", file->private_data);
	file->private_data = kzalloc(sizeof(struct aifs_file_info), GFP_KERNEL);
	if (!AIFS_F(file)) {
		err = -ENOMEM;
		goto out_err;
	}

	/* open lower object and link aifs's file struct to lower's */
	aifs_get_lower_path(file->f_path.dentry, &lower_path);
#if defined(AIFS_DEBUG)
	dentry_path_raw(lower_path.dentry, __tmp, sizeof(__tmp));
	pr_info("aifs, getting lower path dentry: %s\n", __tmp);
#endif
	lower_file = dentry_open(&lower_path, file->f_flags | O_NOATIME, current_cred());
	// lower_file = d_real(lower_path.dentry, NULL, file->f_flags | O_NOATIME, 0);
	path_put(&lower_path);
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		pr_err("lower_file dentry_open error: %d\n", err);
		lower_file = aifs_lower_file(file);
		if (lower_file) {
			aifs_set_lower_file(file, NULL);
			fput(lower_file); /* fput calls dput for lower_dentry */
		}
	} else {
		aifs_set_lower_file(file, lower_file);
#if defined(AIFS_DEBUG)
		dentry_path_raw(lower_file->f_path.dentry,
			AIFS_F(file)->fullpath, sizeof(AIFS_F(file)->fullpath));
		pr_info("aifs: lower file dentry [%s] [err: %d]\n",
				AIFS_F(file)->fullpath, err);
#endif
	}

	if (err) {
		pr_err("some error occured: %d\n", err);
		kfree(AIFS_F(file));
	}
	else
		fsstack_copy_attr_all(inode, aifs_lower_inode(inode));
out_err:
	return err;
}

static int aifs_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = aifs_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush) {
		filemap_write_and_wait(file->f_mapping);
		err = lower_file->f_op->flush(lower_file, id);
	}

	return err;
}

/* release all lower object references & free the file info structure */
static int aifs_file_release(struct inode *inode, struct file *file)
{
	struct file *lower_file;

	lower_file = aifs_lower_file(file);
	if (lower_file) {
		aifs_set_lower_file(file, NULL);
		fput(lower_file);
	}

	kfree(AIFS_F(file));
	return 0;
}

static int aifs_fsync(struct file *file, loff_t start, loff_t end,
			int datasync)
{
	int err;
	struct file *lower_file;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;

	err = __generic_file_fsync(file, start, end, datasync);
	if (err)
		goto out;
	lower_file = aifs_lower_file(file);
	aifs_get_lower_path(dentry, &lower_path);
	err = vfs_fsync_range(lower_file, start, end, datasync);
	aifs_put_lower_path(dentry, &lower_path);
out:
	return err;
}

static int aifs_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = aifs_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);

	return err;
}

/*
 * Wrapfs cannot use generic_file_llseek as ->llseek, because it would
 * only set the offset of the upper file.  So we have to implement our
 * own method to set both the upper and lower file offsets
 * consistently.
 */
static loff_t aifs_file_llseek(struct file *file, loff_t offset, int whence)
{
	int err;
	struct file *lower_file;

	err = generic_file_llseek(file, offset, whence);
	if (err < 0)
		goto out;

	lower_file = aifs_lower_file(file);
	err = generic_file_llseek(lower_file, offset, whence);

out:
	return err;
}

/*
 * Wrapfs read_iter, redirect modified iocb to lower read_iter
 */
ssize_t
aifs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;

	lower_file = aifs_lower_file(file);
	if (!lower_file->f_op->read_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->read_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);
	/* update upper inode atime as needed */
	if (err >= 0 || err == -EIOCBQUEUED)
		fsstack_copy_attr_atime(d_inode(file->f_path.dentry),
					file_inode(lower_file));
out:
	return err;
}

/*
 * Wrapfs write_iter, redirect modified iocb to lower write_iter
 */
ssize_t
aifs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;

	lower_file = aifs_lower_file(file);
	if (!lower_file->f_op->write_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->write_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);
	/* update upper inode times/sizes as needed */
	if (err >= 0 || err == -EIOCBQUEUED) {
		fsstack_copy_inode_size(d_inode(file->f_path.dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(file->f_path.dentry),
					file_inode(lower_file));
	}
out:
	return err;
}

const struct file_operations aifs_main_fops = {
	.llseek		= generic_file_llseek,
#if CONFIG_AIFS_LEGACY_READ
	.read		= aifs_read,
	.write		= aifs_write,
#endif
	.unlocked_ioctl	= aifs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= aifs_compat_ioctl,
#endif
	.mmap		= aifs_mmap,
	.open		= aifs_open,
	.flush		= aifs_flush,
	.release	= aifs_file_release,
	.fsync		= aifs_fsync,
	.fasync		= aifs_fasync,
	.read_iter	= aifs_read_iter,
	.write_iter	= aifs_write_iter,
};

/* trimmed directory options */
const struct file_operations aifs_dir_fops = {
	.llseek		= aifs_file_llseek,
	.iterate	= aifs_readdir,
	.unlocked_ioctl	= aifs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= aifs_compat_ioctl,
#endif
	.open		= aifs_open,
	.release	= aifs_file_release,
	.flush		= aifs_flush,
	.fsync		= aifs_fsync,
	.fasync		= aifs_fasync,
};
