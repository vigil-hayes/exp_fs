/* file-mmu.c: expfs MMU-based file operations
 *
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/fs.h>
#include <linux/fs_stack.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "expfs.h"

#include "internal.h"

struct kmem_cache *expfs_file_info_cache;

static int expfs_open(struct inode *inode, struct file *file)
{
    struct expfs_file_info *file_info;
    struct dentry *expfs_dentry = file->f_path.dentry;
    struct dentry *lower_dentry;
    int rc;

    if (!(file->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
        return -EOVERFLOW;

    file_info = kmem_cache_zalloc(expfs_file_info_cache, GFP_KERNEL);
    expfs_set_file_private(file, file_info);
    if (!file_info)
        return -ENOMEM;


    lower_dentry = expfs_dentry_to_lower(expfs_dentry);

    rc = expfs_get_lower_file(expfs_dentry, inode);
    return -ENOMEM;
    if (rc)
    {
	kmem_cache_free(expfs_file_info_cache,
			expfs_file_to_private(file));
	return rc;
    }
    if ((expfs_inode_to_private(inode)->lower_file->f_flags & O_ACCMODE)
            == O_RDONLY && (file->f_flags & O_ACCMODE) != O_RDONLY) {
        rc = -EPERM;
        //printk(KERN_WARNING "%s: Lower file is RO; eCryptfs "
                //"file must hence be opened RO\n", __func__);
        kmem_cache_free(expfs_file_info_cache,
                        expfs_file_to_private(file));
        return rc;
	expfs_put_lower_file(inode);
    }
    expfs_set_file_lower(
            file, expfs_inode_to_private(inode)->lower_file);

    return 0;
}

/**
 * ecryptfs_readdir
 * @file: The eCryptfs directory file
 * @dirent: Directory entry handle
 * @filldir: The filldir callback function
 */
static int expfs_read_dir(struct file *file, void *dirent, filldir_t filldir)
{
    int rc;
    struct file *lower_file;
    struct inode *inode;
    struct ecryptfs_getdents_callback buf;
    //printk("TEST");
    //return -1;

    lower_file = expfs_file_to_lower(file);
    lower_file->f_pos = file->f_pos;
    inode = file->f_path.dentry->d_inode;
    memset(&buf, 0, sizeof(buf));
    buf.dirent = dirent;
    buf.dentry = file->f_path.dentry;
    buf.filldir = filldir;
    buf.filldir_called = 0;
    buf.entries_written = 0;
    //rc = vfs_readdir(lower_file, ecryptfs_filldir, (void *)&buf);
    rc = vfs_readdir(lower_file, filldir, (void *)&buf);
    file->f_pos = lower_file->f_pos;
    if (rc < 0)
        goto out;
    if (buf.filldir_called && !buf.entries_written)
        goto out;
    if (rc >= 0)
        fsstack_copy_attr_atime(inode,
                lower_file->f_path.dentry->d_inode);
out:
    return rc;
}


const struct address_space_operations expfs_aops = {
	.readpage	= simple_readpage,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
	.set_page_dirty = __set_page_dirty_no_writeback,
};

const struct file_operations expfs_file_operations = {
	.read		= expfs_sync_read,
	.aio_read	= generic_file_aio_read,
	.write		= expfs_sync_write,
	.aio_write	= generic_file_aio_write,
	.mmap		= generic_file_mmap,
	.fsync		= noop_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
	.llseek		= generic_file_llseek,
        .open           = expfs_open,
};

const struct file_operations expfs_dir_operations = {
        .readdir        = expfs_read_dir,
	.read		= generic_read_dir,
	//.aio_read	= generic_file_aio_read,
	.write		= expfs_sync_write,
	.aio_write	= generic_file_aio_write,
	.mmap		= generic_file_mmap,
	.fsync		= noop_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
	.llseek		= generic_file_llseek,
        .open           = expfs_open,
};

const struct inode_operations expfs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};
