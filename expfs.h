#ifndef _LINUX_EXPFS_H
#define _LINUX_EXPFS_H

//struct inode *expfs_get_inode(struct super_block *sb, const struct inode *dir,
	 //umode_t mode, dev_t dev);
extern struct dentry *expfs_mount(struct file_system_type *fs_type,
	 int flags, const char *dev_name, void *data);

#ifndef CONFIG_MMU
extern int expfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize);
extern unsigned long expfs_nommu_get_unmapped_area(struct file *file,
						   unsigned long addr,
						   unsigned long len,
						   unsigned long pgoff,
						   unsigned long flags);

extern int expfs_nommu_mmap(struct file *file, struct vm_area_struct *vma);
#endif

extern const struct file_operations expfs_file_operations;
extern const struct vm_operations_struct generic_file_vm_ops;
extern int __init init_rootfs(void);

int expfs_fill_super(struct super_block *sb, void *data, int silent);
ssize_t expfs_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos);
ssize_t expfs_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos);

extern struct kmem_cache *expfs_dentry_info_cache;
extern struct kmem_cache *expfs_fs_info_cache;
extern struct kmem_cache *expfs_file_info_cache;

struct expfs_mount_opts {
	umode_t mode;
};

struct expfs_inode_info {
    struct inode vfs_inode;
    struct inode *wii_inode;
    struct mutex lower_file_mutex;
    atomic_t lower_file_count;
    struct file *lower_file;
    //struct ecryptfs_crypt_stat crypt_stat;
};

struct expfs_file_info {
    struct file *wfi_file;
    //struct ecryptfs_crypt_stat *crypt_stat;
};

struct expfs_dentry_info {
	struct path lower_path;
	//struct ecryptfs_crypt_stat *crypt_stat;
};

struct expfs_fs_info {
	struct expfs_mount_opts mount_opts;
        struct super_block *lower_sb;
};

static inline struct expfs_inode_info *
expfs_inode_to_private(struct inode *inode)
{
    return container_of(inode, struct expfs_inode_info, vfs_inode);
}

static inline struct inode *expfs_inode_to_lower(struct inode *inode)
{
    return expfs_inode_to_private(inode)->wii_inode;
}

static inline void
expfs_set_inode_lower(struct inode *inode, struct inode *lower_inode)
{
    expfs_inode_to_private(inode)->wii_inode = lower_inode;
}

static inline struct super_block *
expfs_superblock_to_lower(struct super_block *sb)
{
    return ((struct expfs_fs_info *)sb->s_fs_info)->lower_sb;
}

static inline struct expfs_file_info *
expfs_file_to_private(struct file *file)
{
    return file->private_data;
}

static inline void
expfs_set_file_private(struct file *file,
        struct expfs_file_info *file_info)
{
    file->private_data = file_info;
}

static inline struct file *expfs_file_to_lower(struct file *file)
{
    return ((struct expfs_file_info *)file->private_data)->wfi_file;
}

static inline void
expfs_set_file_lower(struct file *file, struct file *lower_file)
{
    ((struct expfs_file_info *)file->private_data)->wfi_file =
        lower_file;
}

static inline struct expfs_dentry_info *
expfs_dentry_to_private(struct dentry *dentry)
{
    return (struct expfs_dentry_info *)dentry->d_fsdata;
}

static inline void
expfs_set_dentry_private(struct dentry *dentry,
        struct expfs_dentry_info *dentry_info)
{
    dentry->d_fsdata = dentry_info;
}

static inline struct dentry *
expfs_dentry_to_lower(struct dentry *dentry)
{
    return ((struct expfs_dentry_info *)dentry->d_fsdata)->lower_path.dentry;
}

static inline struct vfsmount *
expfs_dentry_to_lower_mnt(struct dentry *dentry)
{
    return ((struct expfs_dentry_info *)dentry->d_fsdata)->lower_path.mnt;
}

static inline void
expfs_set_dentry_lower(struct dentry *dentry, struct dentry *lower_dentry)
{
    ((struct expfs_dentry_info *)dentry->d_fsdata)->lower_path.dentry =
        lower_dentry;
}

static inline void
expfs_set_dentry_lower_mnt(struct dentry *dentry, struct vfsmount *lower_mnt)
{
	((struct expfs_dentry_info *)dentry->d_fsdata)->lower_path.mnt =
		lower_mnt;
}

//struct kmem_cache *expfs_open_req_cache;

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
			     const struct cred *cred);

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
                                            struct file **lower_file);

int expfs_get_lower_file(struct dentry *dentry, struct inode *inode);

void expfs_put_lower_file(struct inode *inode);

#endif
