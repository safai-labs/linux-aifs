#include "aifs.h"
#include "dir.h"

int aifs_create_real(struct inode *dir, struct dentry *newdentry,
		    struct cattr *attr,
		    struct dentry *hardlink, bool debug) 
{

	int err;
        if (newdentry->d_inode)
                return -ESTALE;

        if (hardlink) {
                err = aifs_do_link(hardlink, dir, newdentry, debug);
        } else {
                switch (attr->mode & S_IFMT) {
                case S_IFREG:
                        err = aifs_do_create(dir, newdentry, attr->mode, debug);
                        break;

                case S_IFDIR:
                        err = aifs_do_mkdir(dir, newdentry, attr->mode, debug);
                        break;

                case S_IFCHR:
                case S_IFBLK:
                case S_IFIFO:
                case S_IFSOCK:
                        err = aifs_do_mknod(dir, newdentry,
                                           attr->mode, attr->rdev, debug);
                        break;

                case S_IFLNK:
                        err = aifs_do_symlink(dir, newdentry, attr->link, debug);
                        break;

                default:
                        err = -EPERM;
                }
        }
        if (!err && WARN_ON(!newdentry->d_inode)) {
                /*
                 * Not quite sure if non-instantiated dentry is legal or not.
                 * VFS doesn't seem to care so check and warn here.
                 */
                err = -ENOENT;
        }
	return err;
}

