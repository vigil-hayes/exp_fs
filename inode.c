/*
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
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include "expfs.h"
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <asm/uaccess.h>
#include "internal.h"

#define RAMFS_DEFAULT_MODE	0755

struct kmem_cache *expfs_dentry_info_cache;
struct kmem_cache *expfs_fs_info_cache;

static const struct super_operations expfs_ops;
static const struct inode_operations expfs_dir_inode_operations;

static struct backing_dev_info expfs_backing_dev_info = {
	.name		= "expfs",
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK |
			  BDI_CAP_MAP_DIRECT | BDI_CAP_MAP_COPY |
			  BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP,
};

static struct dentry *lock_parent(struct dentry *dentry)
{
    struct dentry *dir;

    dir = dget_parent(dentry);
    mutex_lock_nested(&(dir->d_inode->i_mutex), I_MUTEX_PARENT);
    return dir;
}

static void unlock_dir(struct dentry *dir)
{
    mutex_unlock(&dir->d_inode->i_mutex);
    dput(dir);
}

static int expfs_inode_test(struct inode *inode, void *lower_inode)
{
    if (expfs_inode_to_lower(inode) == (struct inode *)lower_inode)
        return 1;
    return 0;
}

static int expfs_inode_set(struct inode *inode, void *opaque)
{
    struct inode *lower_inode = opaque;

    expfs_set_inode_lower(inode, lower_inode);
    fsstack_copy_attr_all(inode, lower_inode);
    /* i_size will be overwritten for encrypted regular files */
    fsstack_copy_inode_size(inode, lower_inode);
    inode->i_ino = lower_inode->i_ino;
    inode->i_version++;
    inode->i_mapping->a_ops = &expfs_aops;
    inode->i_mapping->backing_dev_info = inode->i_sb->s_bdi;

    if (S_ISLNK(inode->i_mode))
        inode->i_op = &expfs_file_inode_operations;
    else if (S_ISDIR(inode->i_mode))
        inode->i_op = &expfs_dir_inode_operations;
    else if (S_ISLNK(inode->i_mode))
        inode->i_op = &page_symlink_inode_operations;
    /*else
        init_special_inode(inode, mode, dev);*/

    if (S_ISDIR(inode->i_mode))
        inode->i_fop = &simple_dir_operations;
    else if (special_file(inode->i_mode))
        init_special_inode(inode, inode->i_mode, inode->i_rdev);
    else
        inode->i_fop = &expfs_file_operations;

    return 0;
}

static struct inode *__expfs_get_inode(struct inode *lower_inode,
        struct super_block *sb)
{
    struct inode *inode;

    if (lower_inode->i_sb != expfs_superblock_to_lower(sb))
        return ERR_PTR(-EXDEV);
    if (!igrab(lower_inode))
        return ERR_PTR(-ESTALE);
    inode = iget5_locked(sb, (unsigned long)lower_inode,
            expfs_inode_test, expfs_inode_set,
            lower_inode);
    if (!inode) {
        iput(lower_inode);
        return ERR_PTR(-EACCES);
    }
    if (!(inode->i_state & I_NEW))
        iput(lower_inode);

    return inode;
}

struct inode *expfs_get_inode(struct inode *lower_inode,
        struct super_block *sb)
{
    struct inode *inode = __expfs_get_inode(lower_inode, sb);

    if (!IS_ERR(inode) && (inode->i_state & I_NEW))
        unlock_new_inode(inode);

    return inode;
}

//struct inode *expfs_get_inode(struct super_block *sb,
				//const struct inode *dir, umode_t mode, dev_t dev)
//{
	//struct inode * inode = new_inode(sb);
//
	//if (inode) {
		//inode->i_ino = get_next_ino();
		//inode_init_owner(inode, dir, mode);
		//inode->i_mapping->a_ops = &expfs_aops;
		//inode->i_mapping->backing_dev_info = &expfs_backing_dev_info;
		//mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		//mapping_set_unevictable(inode->i_mapping);
		//inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		//switch (mode & S_IFMT) {
		//default:
			//init_special_inode(inode, mode, dev);
			//break;
		//case S_IFREG:
			//inode->i_op = &expfs_file_inode_operations;
			//inode->i_fop = &expfs_file_operations;
			//break;
		//case S_IFDIR:
			//inode->i_op = &expfs_dir_inode_operations;
			//inode->i_fop = &simple_dir_operations;
//
			///* directory inodes start off with i_nlink == 2 (for "." entry) */
			//inc_nlink(inode);
			//break;
		//case S_IFLNK:
			//inode->i_op = &page_symlink_inode_operations;
			//break;
		//}
	//}
	//return inode;
//}

static int expfs_i_size_read(struct dentry *dentry, struct inode *inode)
{
	//struct ecryptfs_crypt_stat *crypt_stat;
	int rc;

	rc = expfs_get_lower_file(dentry, inode);
	if (rc) {
		//printk(KERN_ERR "%s: Error attempting to initialize "
			//"the lower file for the dentry with name "
			//"[%s]; rc = [%d]\n", __func__,
			//dentry->d_name.name, rc);
		return rc;
	}

	//crypt_stat = &expfs_inode_to_private(inode)->crypt_stat;
	/* TODO: lock for crypt_stat comparison */
	//if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED))
		//ecryptfs_set_default_sizes(crypt_stat);

	//rc = ecryptfs_read_and_validate_header_region(inode);
	expfs_put_lower_file(inode);
	if (rc) {
		//rc = ecryptfs_read_and_validate_xattr_region(dentry, inode);
		//if (!rc)
			//crypt_stat->flags |= ECRYPTFS_METADATA_IN_XATTR;
	}

	/* Must return 0 to allow non-eCryptfs files to be looked up, too */
	return 0;
}


static int expfs_lookup_interpose(struct dentry *dentry,
				     struct dentry *lower_dentry,
				     struct inode *dir_inode)
{
	struct inode *inode, *lower_inode = lower_dentry->d_inode;
	struct expfs_dentry_info *dentry_info;
	struct vfsmount *lower_mnt;
	int rc = 0;

	lower_mnt = mntget(expfs_dentry_to_lower_mnt(dentry->d_parent));
	fsstack_copy_attr_atime(dir_inode, lower_dentry->d_parent->d_inode);
	BUG_ON(!lower_dentry->d_count);

	dentry_info = kmem_cache_alloc(expfs_dentry_info_cache, GFP_KERNEL);
	expfs_set_dentry_private(dentry, dentry_info);
	if (!dentry_info) {
		printk(KERN_ERR "%s: Out of memory whilst attempting "
		       "to allocate ecryptfs_dentry_info struct\n",
			__func__);
		dput(lower_dentry);
		mntput(lower_mnt);
		d_drop(dentry);
		return -ENOMEM;
	}
	expfs_set_dentry_lower(dentry, lower_dentry);
	expfs_set_dentry_lower_mnt(dentry, lower_mnt);

	if (!lower_dentry->d_inode) {
		/* We want to add because we couldn't find in lower */
		d_add(dentry, NULL);
		return 0;
	}
	inode = __expfs_get_inode(lower_inode, dir_inode->i_sb);
	if (IS_ERR(inode)) {
		printk(KERN_ERR "%s: Error interposing; rc = [%ld]\n",
		       __func__, PTR_ERR(inode));
		return PTR_ERR(inode);
	}
	if (S_ISREG(inode->i_mode)) {
		rc = expfs_i_size_read(dentry, inode);
		if (rc) {
			make_bad_inode(inode);
			return rc;
		}
	}

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);
	d_add(dentry, inode);

	return rc;
}

/**
 * ecryptfs_lookup
 * @ecryptfs_dir_inode: The eCryptfs directory inode
 * @ecryptfs_dentry: The eCryptfs dentry that we are looking up
 * @ecryptfs_nd: nameidata; may be NULL
 *
 * Find a file on disk. If the file does not exist, then we'll add it to the
 * dentry cache and continue on to read it from the disk.
 */
static struct dentry *expfs_lookup(struct inode *expfs_dir_inode,
				      struct dentry *expfs_dentry,
				      struct nameidata *expfs_nd)
{
	//char *encrypted_and_encoded_name = NULL;
	//size_t encrypted_and_encoded_name_size;
	//struct ecryptfs_mount_crypt_stat *mount_crypt_stat = NULL;
	struct dentry *lower_dir_dentry, *lower_dentry;
	int rc = 0;

	if ((expfs_dentry->d_name.len == 1
	     && !strcmp(expfs_dentry->d_name.name, "."))
	    || (expfs_dentry->d_name.len == 2
		&& !strcmp(expfs_dentry->d_name.name, ".."))) {
		goto out_d_drop;
	}
	lower_dir_dentry = expfs_dentry_to_lower(expfs_dentry->d_parent);
	mutex_lock(&lower_dir_dentry->d_inode->i_mutex);
	lower_dentry = lookup_one_len(expfs_dentry->d_name.name,
				      lower_dir_dentry,
				      expfs_dentry->d_name.len);
	mutex_unlock(&lower_dir_dentry->d_inode->i_mutex);
	if (IS_ERR(lower_dentry)) {
		rc = PTR_ERR(lower_dentry);
		//ecryptfs_printk(KERN_DEBUG, "%s: lookup_one_len() returned "
				//"[%d] on lower_dentry = [%s]\n", __func__, rc,
				//encrypted_and_encoded_name);
		goto out_d_drop;
	}
	if (lower_dentry->d_inode)
		goto interpose;
	//mount_crypt_stat = &ecryptfs_superblock_to_private(
				//ecryptfs_dentry->d_sb)->mount_crypt_stat;
	//if (!(mount_crypt_stat
	    //&& (mount_crypt_stat->flags & ECRYPTFS_GLOBAL_ENCRYPT_FILENAMES)))
		//goto interpose;
	dput(lower_dentry);
	//rc = ecryptfs_encrypt_and_encode_filename(
		//&encrypted_and_encoded_name, &encrypted_and_encoded_name_size,
		//NULL, mount_crypt_stat, ecryptfs_dentry->d_name.name,
		//ecryptfs_dentry->d_name.len);
	//if (rc) {
		//printk(KERN_ERR "%s: Error attempting to encrypt and encode "
		       //"filename; rc = [%d]\n", __func__, rc);
		//goto out_d_drop;
	//}
	mutex_lock(&lower_dir_dentry->d_inode->i_mutex);
	lower_dentry = lookup_one_len(expfs_dentry->d_name.name,//encrypted_and_encoded_name,
				      lower_dir_dentry,
				      expfs_dentry->d_name.len);//encrypted_and_encoded_name_size);
	mutex_unlock(&lower_dir_dentry->d_inode->i_mutex);
	if (IS_ERR(lower_dentry)) {
		rc = PTR_ERR(lower_dentry);
		//ecryptfs_printk(KERN_DEBUG, "%s: lookup_one_len() returned "
				//"[%d] on lower_dentry = [%s]\n", __func__, rc,
				//encrypted_and_encoded_name);
		goto out_d_drop;
	}
interpose:
	rc = expfs_lookup_interpose(expfs_dentry, lower_dentry,
				       expfs_dir_inode);
	goto out;
out_d_drop:
	d_drop(expfs_dentry);
out:
	//kfree(encrypted_and_encoded_name);
	return ERR_PTR(rc);
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
expfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	//struct inode * inode = expfs_get_inode(dir->i_sb, dir, mode, dev);
	//int error = -ENOSPC;
//
	//if (inode) {
		//d_instantiate(dentry, inode);
		//dget(dentry);	/* Extra count - pin the dentry in core */
		//error = 0;
		//dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	//}
	//return error;
    int rc;
    struct dentry *lower_dentry;
    struct dentry *lower_dir_dentry;

    lower_dentry = expfs_dentry_to_lower(dentry);
    lower_dir_dentry = lock_parent(lower_dentry);
    rc = vfs_mknod(lower_dir_dentry->d_inode, lower_dentry, mode, dev);
    if (rc || !lower_dentry->d_inode)
        goto out;
    //rc = ecryptfs_interpose(lower_dentry, dentry, dir->i_sb);
    if (rc)
        goto out;
    fsstack_copy_attr_times(dir, lower_dir_dentry->d_inode);
    fsstack_copy_inode_size(dir, lower_dir_dentry->d_inode);
out:
    unlock_dir(lower_dir_dentry);
    if (!dentry->d_inode)
        d_drop(dentry);
    return rc;
}

static int expfs_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = expfs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int expfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, struct nameidata *nd)
{
	return expfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int expfs_symlink(struct inode *dir, struct dentry *dentry,
        const char *symname)
{
    int rc;
    struct dentry *lower_dentry;
    struct dentry *lower_dir_dentry;
    //char *encoded_symname;
    //size_t encoded_symlen;
    //struct ecryptfs_mount_crypt_stat *mount_crypt_stat = NULL;

    lower_dentry = expfs_dentry_to_lower(dentry);
    dget(lower_dentry);
    lower_dir_dentry = lock_parent(lower_dentry);
    //mount_crypt_stat = &ecryptfs_superblock_to_private(
            //dir->i_sb)->mount_crypt_stat;
    //rc = ecryptfs_encrypt_and_encode_filename(&encoded_symname,
            //&encoded_symlen,
            //NULL,
            //mount_crypt_stat, symname,
            //strlen(symname));
    //if (rc)
        //goto out_lock;
    rc = vfs_symlink(lower_dir_dentry->d_inode, lower_dentry,
            symname);
    //kfree(encoded_symname);
    //if (rc || !lower_dentry->d_inode)
        //goto out_lock;
    //rc = ecryptfs_interpose(lower_dentry, dentry, dir->i_sb);
    //if (rc)
        //goto out_lock;
    fsstack_copy_attr_times(dir, lower_dir_dentry->d_inode);
    fsstack_copy_inode_size(dir, lower_dir_dentry->d_inode);
//out_lock:
    unlock_dir(lower_dir_dentry);
    dput(lower_dentry);
    if (!dentry->d_inode)
        d_drop(dentry);
    return rc;
}

//static int expfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
//{
	//struct inode *inode;
	//int error = -ENOSPC;
//
	//inode = expfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	//if (inode) {
		//int l = strlen(symname)+1;
		//error = page_symlink(inode, symname, l);
		//if (!error) {
			//d_instantiate(dentry, inode);
			//dget(dentry);
			//dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		//} else
			//iput(inode);
	//}
	//return error;
//}

static const struct inode_operations expfs_dir_inode_operations = {
	.create		= expfs_create,
	.lookup		= expfs_lookup,//simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= expfs_symlink,
	.mkdir		= expfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= expfs_mknod,
	.rename		= simple_rename,
};

static const struct super_operations expfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
};

enum {
	Opt_mode,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_mode, "mode=%o"},
	{Opt_err, NULL}
};

static int expfs_parse_options(char *data, struct expfs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

	opts->mode = RAMFS_DEFAULT_MODE;

	//while ((p = strsep(&data, ",")) != NULL) {
		//if (!*p)
			//continue;
//
		//token = match_token(p, tokens, args);
		//switch (token) {
		//case Opt_mode:
			//if (match_octal(&args[0], &option))
				//return -EINVAL;
			//opts->mode = option & S_IALLUGO;
			//break;
		///*
		 //* We might like to report bad mount options here;
		 //* but traditionally ramfs has ignored all mount options,
		 //* and as it is used as a !CONFIG_SHMEM simple substitute
		 //* for tmpfs, better continue to ignore other mount options.
		 //*/
		//}
	//}

	return 0;
}

//int expfs_fill_super(struct super_block *sb, void *data, int silent)
//{
	//struct expfs_fs_info *fsi;
	//struct inode *inode;
	//struct path path;
	//int err;
	//int rc /*= kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path)*/;
//
	//if (rc) {
		////printk(KERN_WARNING, "kern_path() failed\n");
                //deactivate_locked_super(sb);
                //return (int)ERR_PTR(rc);
		////goto out1;
	//}
	//save_mount_options(sb, data);
//
	//fsi = kzalloc(sizeof(struct expfs_fs_info), GFP_KERNEL);
	//sb->s_fs_info = fsi;
	//if (!fsi)
		//return -ENOMEM;
//
	//err = expfs_parse_options(data, &fsi->mount_opts);
	//if (err)
		//return err;
//
	//fsi->lower_sb = path.dentry->d_sb;
	//sb->s_maxbytes = path.dentry->d_sb->s_maxbytes;
	//sb->s_blocksize = path.dentry->d_sb->s_blocksize;
	////sb->s_magic = ECRYPTFS_SUPER_MAGIC;
	////sb->s_maxbytes		= MAX_LFS_FILESIZE;
	////sb->s_blocksize		= PAGE_CACHE_SIZE;
	//sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	//sb->s_magic		= RAMFS_MAGIC;
	//sb->s_op		= &expfs_ops;
	//sb->s_time_gran		= 1;
//
	//inode = expfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	//sb->s_root = d_make_root(inode);
	//if (!sb->s_root)
		//return -ENOMEM;
//
	//return 0;
//}


/* Maybe modify */
struct dentry *expfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
    //return mount_nodev(fs_type, flags, data, expfs_fill_super);
    int err;
    struct path path;
    struct super_block *s;
    struct expfs_fs_info *fsi;
    struct expfs_dentry_info *root_info;
    struct inode *inode;
    int rc;
    char *tmp;
   
    rc = kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
    //printk("PATH:%s\n",path.dentry->d_name.name);
    //rc = -ENOMEM; goto out;
    s = sget(fs_type, NULL, set_anon_super, NULL);
    if (IS_ERR(s)) {
        rc = PTR_ERR(s);
        goto out;
    }
    if (rc) {
            //printk(KERN_WARNING, "kern_path() failed\n");
            goto out1;
    }
    //save_mount_options(s, data);
    /*if (path.dentry->d_sb->s_type == &expfs_fs_type) {
            rc = -EINVAL;
            printk(KERN_ERR "Mount on filesystem of type "
                    "expfs explicitly disallowed due to "
                    "known incompatibilities\n");
            goto out_free;
    }*/
    /*if (check_ruid && path.dentry->d_inode->i_uid != current_uid()) {
            rc = -EPERM;
            printk(KERN_ERR "Mount of device (uid: %d) not owned by "
                   "requested user (uid: %d)\n",
                   path.dentry->d_inode->i_uid, current_uid());
            goto out_free;
    }*/
    //fsi = kzalloc(sizeof(struct expfs_fs_info), GFP_KERNEL);
    //s->s_fs_info = fsi;
    //if (!fsi)
            //return (struct dentry*)(-ENOMEM);

    fsi = kmem_cache_zalloc(expfs_fs_info_cache, GFP_KERNEL);
    if (!fsi) {
        rc = -ENOMEM;
        goto out;
    }

    s->s_fs_info = fsi;

    err = expfs_parse_options(data, &fsi->mount_opts);
    if (err)
            return (struct dentry*)err;

    fsi->lower_sb = path.dentry->d_sb;
    s->s_maxbytes = path.dentry->d_sb->s_maxbytes;
    s->s_blocksize = path.dentry->d_sb->s_blocksize;
    //s->s_magic = ECRYPTFS_SUPER_MAGIC;
    s->s_blocksize_bits	= PAGE_CACHE_SHIFT;
    s->s_magic		= RAMFS_MAGIC;
    s->s_op		= &expfs_ops;
    s->s_time_gran		= 1;

    //inode = expfs_get_inode(s, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
    inode = expfs_get_inode(path.dentry->d_inode, s);
    s->s_root = d_make_root(inode);
    if (!s->s_root)
            return (struct dentry*)(-ENOMEM);

//
    //inode = ecryptfs_get_inode(path.dentry->d_inode, s);
    //rc = PTR_ERR(inode);
    //if (IS_ERR(inode))
            //goto out_free;
//
    //s->s_root = d_make_root(inode);
    //if (!s->s_root) {
            //rc = -ENOMEM;
            //goto out_free;
    //}
//
    //rc = -ENOMEM;
    root_info = kmem_cache_zalloc(expfs_dentry_info_cache, GFP_KERNEL);
    if (!root_info)
            goto out_free;
//
    ///* ->kill_sb() will take care of root_info */
    expfs_set_dentry_private(s->s_root, root_info);
    expfs_set_dentry_lower(s->s_root, path.dentry);
    expfs_set_dentry_lower_mnt(s->s_root, path.mnt);
    //tmp = ((struct expfs_dentry_info*)(s->s_root->d_fsdata))->lower_path.dentry->d_name.name;
    //printk("PATH:%s\n",tmp);
    //rc = -ENOMEM; goto out;
    return dget(s->s_root);
out_free:
    path_put(&path);
out1:
    deactivate_locked_super(s);
out:
    //if (sbi) {
            ////ecryptfs_destroy_mount_crypt_stat(&sbi->mount_crypt_stat);
            ////kmem_cache_free(ecryptfs_sb_info_cache, sbi);
    //}
    //printk(KERN_ERR "%s; rc = [%d]\n", err, rc);
    return ERR_PTR(rc);
}


/*static struct dentry *rootfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags|MS_NOUSER, data, expfs_fill_super);
}*/


static void expfs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}


static struct file_system_type expfs_fs_type = {
	.name		= "expfs",
	.mount		= expfs_mount,
	.kill_sb	= expfs_kill_sb,
};
/*static struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.mount		= rootfs_mount,
	.kill_sb	= kill_litter_super,
};*/

static int __init init_expfs_fs(void)
{
	return register_filesystem(&expfs_fs_type);
}
module_init(init_expfs_fs)

//static struct dentry expfs_lookup(struct inode *expfs_dir_inode,
        //struct dentry *expfs_dentry,
        //struct nameidata *expfs_nd)
//{
    //struct dentry *lower_dir_dentry = expfs_dentry_to_lower(ecryptfs_dentry->d_parent);
//}

ssize_t expfs_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    if (false)
    {
        if (*ppos > 0 || len < 3)
        {
            *ppos = 3;
            return 0;
        }
        else
        {
            buf[0] = 'a';
            buf[1] = 'b';
            buf[2] = 'c';
            *ppos = 3;
            //buf[*(ppos)++] = '\0';
            return 3;
        }
    }
    else
    {
        ////return do_sync_read(filp,buf,len,ppos);
        ////printk("from pos:%lld ",*ppos);
        //ssize_t ret = do_sync_read(filp,buf,len,ppos);
        //int i;
        //for(i = 0; i < ret; i++)
        //{
            //if (buf[i] >= 'A' && buf[i] <= 'z')
                //buf[i]++;
            //else if (buf[i] >= '0' && buf[i] <= '9')
                //buf[i] = '9' - (buf[i] - '0');
        //}
        ////printk("to pos:%lld;ret:%d\n",*ppos,ret);
        //return ret;
        struct file *lower_file;
        mm_segment_t fs_save;
        ssize_t rc;

        lower_file = expfs_file_to_private(filp)->wfi_file;
        if (!lower_file)
            return -EIO;
        fs_save = get_fs();
        set_fs(get_ds());
        rc = vfs_read(lower_file, buf, len, ppos);
        set_fs(fs_save);
        return rc;
    }
}

ssize_t expfs_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
    //char *buf_ = kmalloc(len,GFP_KERNEL);
    //char *buf_ = malloc(len);
    //int i;
    //for(i = 0; i < len / 2; i++)
    //{
        //buf_[i] = buf[len - i - 1];
        //buf_[len - i - 1] = buf[i];
    //}
    //if (len & 1)
        //buf_[len/2] = buf[len/2];
    char *buf_ = kmalloc(5,GFP_KERNEL);
    ssize_t ret ;
    buf_[0] = 'a';
    buf_[1] = 'b';
    buf_[2] = 'C';
    buf_[3] = 'D';
    buf_[4] = '\n';
    //len = 5;
    printk("write buffer:%d\n",len);
    ret = do_sync_write(filp, buf, len, ppos);
    printk("successfully wrote buffer\n");
    kfree(buf_);
    //free(buf_);
    return ret;
}



/**
 * ecryptfs_privileged_open
 * @lower_file: Result of dentry_open by root on lower dentry
 * @lower_dentry: Lower dentry for file to open
 * @lower_mnt: Lower vfsmount for file to open
 *
 * This function gets a r/w file opened againt the lower dentry.
 *
 * Returns zero on success; non-zero otherwise
 */
int expfs_privileged_open(struct file **lower_file,
			     struct dentry *lower_dentry,
			     struct vfsmount *lower_mnt,
			     const struct cred *cred)
{
	//struct expfs_open_req *req;
	int flags = O_LARGEFILE;
	int rc = 0;

	/* Corresponding dput() and mntput() are done when the
	 * lower file is fput() when all eCryptfs files for the inode are
	 * released. */
	dget(lower_dentry);
	mntget(lower_mnt);
	flags |= IS_RDONLY(lower_dentry->d_inode) ? O_RDONLY : O_RDWR;
	(*lower_file) = dentry_open(lower_dentry, lower_mnt, flags, cred);
	if (!IS_ERR(*lower_file))
		goto out;
	if (flags & O_RDONLY) {
		rc = PTR_ERR((*lower_file));
		goto out;
	}
        dget(lower_dentry);
        mntget(lower_mnt);
        (*lower_file) = dentry_open(
                lower_dentry, lower_mnt,
                (O_RDWR | O_LARGEFILE), current_cred());
	//req = kmem_cache_alloc(ecryptfs_open_req_cache, GFP_KERNEL);
	//if (!req) {
		//rc = -ENOMEM;
		//goto out;
	//}
	//mutex_init(&req->mux);
	//req->lower_file = lower_file;
	//req->lower_dentry = lower_dentry;
	//req->lower_mnt = lower_mnt;
	//init_waitqueue_head(&req->wait);
	//req->flags = 0;
	//mutex_lock(&ecryptfs_kthread_ctl.mux);
	//if (expfs_kthread_ctl.flags & ECRYPTFS_KTHREAD_ZOMBIE) {
		//rc = -EIO;
		//mutex_unlock(&ecryptfs_kthread_ctl.mux);
		//printk(KERN_ERR "%s: We are in the middle of shutting down; "
		       //"aborting privileged request to open lower file\n",
			//__func__);
		//goto out_free;
	//}
	//list_add_tail(&req->kthread_ctl_list, &ecryptfs_kthread_ctl.req_list);
	//mutex_unlock(&ecryptfs_kthread_ctl.mux);
	//wake_up(&ecryptfs_kthread_ctl.wait);
	//wait_event(req->wait, (req->flags != 0));
	//mutex_lock(&req->mux);
	//BUG_ON(req->flags == 0);
	//if (req->flags & ECRYPTFS_REQ_DROPPED
	    //|| req->flags & ECRYPTFS_REQ_ZOMBIE) {
		//rc = -EIO;
		//printk(KERN_WARNING "%s: Privileged open request dropped\n",
		       //__func__);
		//goto out_unlock;
	//}
	//if (IS_ERR(*req->lower_file))
		//rc = PTR_ERR(*req->lower_file);
//out_unlock:
	//mutex_unlock(&req->mux);
//out_free:
	//kmem_cache_free(ecryptfs_open_req_cache, req);
out:
	return rc;
}

/**
 * ecryptfs_init_lower_file
 * @ecryptfs_dentry: Fully initialized eCryptfs dentry object, with
 *                   the lower dentry and the lower mount set
 *
 * eCryptfs only ever keeps a single open file for every lower
 * inode. All I/O operations to the lower inode occur through that
 * file. When the first eCryptfs dentry that interposes with the first
 * lower dentry for that inode is created, this function creates the
 * lower file struct and associates it with the eCryptfs
 * inode. When all eCryptfs files associated with the inode are released, the
 * file is closed.
 *
 * The lower file will be opened with read/write permissions, if
 * possible. Otherwise, it is opened read-only.
 *
 * This function does nothing if a lower file is already
 * associated with the eCryptfs inode.

 * Returns zero on success; non-zero otherwise
 */
static int expfs_init_lower_file(struct dentry *dentry,
                                            struct file **lower_file)
{
    const struct cred *cred = current_cred();
    struct dentry *lower_dentry = expfs_dentry_to_lower(dentry);
    struct vfsmount *lower_mnt = expfs_dentry_to_lower_mnt(dentry);
    int rc;

    rc = expfs_privileged_open(lower_file, lower_dentry, lower_mnt,
            cred);
    if (rc) {
        //printk(KERN_ERR "Error opening lower file "
                //"for lower_dentry [0x%p] and lower_mnt [0x%p]; "
                //"rc = [%d]\n", lower_dentry, lower_mnt, rc);
        (*lower_file) = NULL;
    }
    return rc;
}

int expfs_get_lower_file(struct dentry *dentry, struct inode *inode)
{
    struct expfs_inode_info *inode_info;
    int count, rc = 0;

    inode_info = expfs_inode_to_private(inode);
    mutex_lock(&inode_info->lower_file_mutex);
    count = atomic_inc_return(&inode_info->lower_file_count);
    if (WARN_ON_ONCE(count < 1))
        rc = -EINVAL;
    else if (count == 1) {
        rc = expfs_init_lower_file(dentry,
                &inode_info->lower_file);
        if (rc)
            atomic_set(&inode_info->lower_file_count, 0);
    }
    mutex_unlock(&inode_info->lower_file_mutex);
    return rc;
}

void expfs_put_lower_file(struct inode *inode)
{
    struct expfs_inode_info *inode_info;

    inode_info = expfs_inode_to_private(inode);
    if (atomic_dec_and_mutex_lock(&inode_info->lower_file_count,
                &inode_info->lower_file_mutex)) {
        fput(inode_info->lower_file);
        inode_info->lower_file = NULL;
        mutex_unlock(&inode_info->lower_file_mutex);
    }
}


static struct expfs_cache_info {
	struct kmem_cache **cache;
	const char *name;
	size_t size;
	void (*ctor)(void *obj);
} expfs_cache_infos[] = {
	/*{
		.cache = &ecryptfs_auth_tok_list_item_cache,
		.name = "ecryptfs_auth_tok_list_item",
		.size = sizeof(struct ecryptfs_auth_tok_list_item),
	},*/
	{
		.cache = &expfs_file_info_cache,
		.name = "expfs_file_cache",
		.size = sizeof(struct expfs_file_info),
	},
	{
		.cache = &expfs_dentry_info_cache,
		.name = "expfs_dentry_info_cache",
		.size = sizeof(struct expfs_dentry_info),
	},
	/*{
		.cache = &expfs_inode_info_cache,
		.name = "expfs_inode_cache",
		.size = sizeof(struct expfs_inode_info),
		.ctor = inode_info_init_once,
	},*/
	{
		.cache = &expfs_fs_info_cache,
		.name = "expfs_sb_cache",
		.size = sizeof(struct expfs_fs_info),
	},
	/*{
		.cache = &ecryptfs_header_cache,
		.name = "ecryptfs_headers",
		.size = PAGE_CACHE_SIZE,
	},
	{
		.cache = &ecryptfs_xattr_cache,
		.name = "ecryptfs_xattr_cache",
		.size = PAGE_CACHE_SIZE,
	},
	{
		.cache = &ecryptfs_key_record_cache,
		.name = "ecryptfs_key_record_cache",
		.size = sizeof(struct ecryptfs_key_record),
	},
	{
		.cache = &ecryptfs_key_sig_cache,
		.name = "ecryptfs_key_sig_cache",
		.size = sizeof(struct ecryptfs_key_sig),
	},
	{
		.cache = &ecryptfs_global_auth_tok_cache,
		.name = "ecryptfs_global_auth_tok_cache",
		.size = sizeof(struct ecryptfs_global_auth_tok),
	},
	{
		.cache = &ecryptfs_key_tfm_cache,
		.name = "ecryptfs_key_tfm_cache",
		.size = sizeof(struct ecryptfs_key_tfm),
	},
	{
		.cache = &ecryptfs_open_req_cache,
		.name = "ecryptfs_open_req_cache",
		.size = sizeof(struct ecryptfs_open_req),
	},*/
};

static void expfs_free_kmem_caches(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(expfs_cache_infos); i++) {
		struct expfs_cache_info *info;

		info = &expfs_cache_infos[i];
		if (*(info->cache))
			kmem_cache_destroy(*(info->cache));
	}
}

/**
 * ecryptfs_init_kmem_caches
 *
 * Returns zero on success; non-zero otherwise
 */
static int expfs_init_kmem_caches(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(expfs_cache_infos); i++) {
		struct expfs_cache_info *info;

		info = &expfs_cache_infos[i];
		*(info->cache) = kmem_cache_create(info->name, info->size,
				0, SLAB_HWCACHE_ALIGN, info->ctor);
		if (!*(info->cache)) {
			expfs_free_kmem_caches();
			//ecryptfs_printk(KERN_WARNING, "%s: "
					//"kmem_cache_create failed\n",
					//info->name);
			return -ENOMEM;
		}
	}
	return 0;
}

//static int do_sysfs_registration(void)
//{
	//int rc;
//
	//expfs_kobj = kobject_create_and_add("ecryptfs", fs_kobj);
	//if (!expfs_kobj) {
		//printk(KERN_ERR "Unable to create ecryptfs kset\n");
		//rc = -ENOMEM;
		//goto out;
	//}
	//rc = sysfs_create_group(ecryptfs_kobj, &attr_group);
	//if (rc) {
		//printk(KERN_ERR
		       //"Unable to create ecryptfs version attributes\n");
		//kobject_put(ecryptfs_kobj);
	//}
//out:
	//return rc;
//}

//static void do_sysfs_unregistration(void)
//{
	//sysfs_remove_group(ecryptfs_kobj, &attr_group);
	//kobject_put(ecryptfs_kobj);
//}

static int __init expfs_init(void)
{
	int rc;

	//if (ECRYPTFS_DEFAULT_EXTENT_SIZE > PAGE_CACHE_SIZE) {
		//rc = -EINVAL;
		//ecryptfs_printk(KERN_ERR, "The eCryptfs extent size is "
				//"larger than the host's page size, and so "
				//"eCryptfs cannot run on this system. The "
				//"default eCryptfs extent size is [%u] bytes; "
				//"the page size is [%lu] bytes.\n",
				//ECRYPTFS_DEFAULT_EXTENT_SIZE,
				//(unsigned long)PAGE_CACHE_SIZE);
		//goto out;
	//}
	rc = expfs_init_kmem_caches();
	if (rc) {
		printk(KERN_ERR
		       "Failed to allocate one or more kmem_cache objects\n");
		goto out;
	}
	//rc = do_sysfs_registration();
	//if (rc) {
		//printk(KERN_ERR "sysfs registration failed\n");
		//goto out_free_kmem_caches;
	//}
	//rc = ecryptfs_init_kthread();
	//if (rc) {
		//printk(KERN_ERR "%s: kthread initialization failed; "
		       //"rc = [%d]\n", __func__, rc);
		//goto out_do_sysfs_unregistration;
	//}
	//rc = ecryptfs_init_messaging();
	//if (rc) {
		//printk(KERN_ERR "Failure occurred while attempting to "
				//"initialize the communications channel to "
				//"ecryptfsd\n");
		//goto out_destroy_kthread;
	//}
	//rc = ecryptfs_init_crypto();
	//if (rc) {
		//printk(KERN_ERR "Failure whilst attempting to init crypto; "
		       //"rc = [%d]\n", rc);
		//goto out_release_messaging;
	//}
	rc = register_filesystem(&expfs_fs_type);
	if (rc) {
		printk(KERN_ERR "Failed to register filesystem\n");
		//goto out_destroy_crypto;
                goto out_free_kmem_caches;
	}
	//if (ecryptfs_verbosity > 0)
		//printk(KERN_CRIT "eCryptfs verbosity set to %d. Secret values "
			//"will be written to the syslog!\n", ecryptfs_verbosity);

	goto out;
//out_destroy_crypto:
	//ecryptfs_destroy_crypto();
//out_release_messaging:
	//ecryptfs_release_messaging();
//out_destroy_kthread:
	//ecryptfs_destroy_kthread();
//out_do_sysfs_unregistration:
	//do_sysfs_unregistration();
out_free_kmem_caches:
	expfs_free_kmem_caches();
out:
	return rc;
}

static void __exit expfs_exit(void)
{
	int rc;

	//rc = ecryptfs_destroy_crypto();
	//if (rc)
		//printk(KERN_ERR "Failure whilst attempting to destroy crypto; "
		       //"rc = [%d]\n", rc);
	//ecryptfs_release_messaging();
	//ecryptfs_destroy_kthread();
	//do_sysfs_unregistration();
	unregister_filesystem(&expfs_fs_type);
	expfs_free_kmem_caches();
}

module_init(expfs_init)
module_exit(expfs_exit)
