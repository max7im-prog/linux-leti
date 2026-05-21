// simplefs.c

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x53465331
#define SIMPLEFS_MAX_FILES 16

/* -------------------------
 * in-memory file storage
 * ------------------------- */
struct simplefs_file {
  char *data;
  size_t size;
};

static struct simplefs_file files[SIMPLEFS_MAX_FILES];

/* -------------------------
 * inode ops
 * ------------------------- */
static struct inode *simplefs_get_inode(struct super_block *sb,
                                        const struct inode *dir, umode_t mode,
                                        int i_ino) {
  struct inode *inode = new_inode(sb);
  if (!inode)
    return NULL;

  inode->i_ino = i_ino;
  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);

  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

  if (S_ISDIR(mode)) {
    inode->i_op = NULL;
    inode->i_fop = NULL;
  } else if (S_ISREG(mode)) {
    inode->i_fop = NULL; // назначим позже per-file
  }

  return inode;
}

/* -------------------------
 * file operations
 * ------------------------- */
static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
                             loff_t *offset) {
  int idx = (int)(long)file->private_data;
  struct simplefs_file *f = &files[idx];

  if (*offset >= f->size)
    return 0;

  if (*offset + len > f->size)
    len = f->size - *offset;

  if (copy_to_user(buf, f->data + *offset, len))
    return -EFAULT;

  *offset += len;
  return len;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
                              size_t len, loff_t *offset) {
  int idx = (int)(long)file->private_data;
  struct simplefs_file *f = &files[idx];

  if (*offset + len > PAGE_SIZE) {
    len = PAGE_SIZE - *offset;
  }

  if (!f->data) {
    f->data = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!f->data)
      return -ENOMEM;
  }

  if (copy_from_user(f->data + *offset, buf, len))
    return -EFAULT;

  *offset += len;
  f->size = max_t(size_t, f->size, *offset);

  return len;
}

static const struct file_operations simplefs_fops = {
    .read = simplefs_read,
    .write = simplefs_write,
    .llseek = default_llseek,
};

/* -------------------------
 * directory listing
 * ------------------------- */
static int simplefs_iterate(struct file *filp, struct dir_context *ctx) {
  int i;

  if (ctx->pos)
    return 0;

  for (i = 0; i < SIMPLEFS_MAX_FILES; i++) {
    char name[32];

    snprintf(name, sizeof(name), "file%d", i);

    if (!dir_emit(ctx, name, strlen(name), i + 1, DT_REG))
      return 0;

    ctx->pos++;
  }

  return 0;
}

static const struct file_operations simplefs_dir_ops = {
    .iterate_shared = simplefs_iterate,
};

/* -------------------------
 * superblock
 * ------------------------- */
static struct inode *simplefs_root_inode(struct super_block *sb) {
  struct inode *inode;

  inode = new_inode(sb);
  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFDIR | 0755);

  inode->i_op = NULL;
  inode->i_fop = &simplefs_dir_ops;
  inode->i_ino = 1;

  return inode;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode *root;

  sb->s_magic = SIMPLEFS_MAGIC;
  sb->s_blocksize = PAGE_SIZE;
  sb->s_op = NULL;

  root = simplefs_root_inode(sb);
  if (!root)
    return -ENOMEM;

  sb->s_root = d_make_root(root);
  if (!sb->s_root)
    return -ENOMEM;

  return 0;
}

/* -------------------------
 * mount
 * ------------------------- */
static struct dentry *simplefs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data) {
  return mount_nodev(fs_type, flags, data, simplefs_fill_super);
}

/* -------------------------
 * fs type
 * ------------------------- */
static struct file_system_type simplefs_fs_type = {
    .owner = THIS_MODULE,
    .name = SIMPLEFS_NAME,
    .mount = simplefs_mount,
    .kill_sb = kill_litter_super,
};

/* -------------------------
 * init / exit
 * ------------------------- */
static int __init simplefs_init(void) {
  int i;

  for (i = 0; i < SIMPLEFS_MAX_FILES; i++) {
    files[i].data = NULL;
    files[i].size = 0;
  }

  return register_filesystem(&simplefs_fs_type);
}

static void __exit simplefs_exit(void) {
  int i;

  for (i = 0; i < SIMPLEFS_MAX_FILES; i++) {
    kfree(files[i].data);
  }

  unregister_filesystem(&simplefs_fs_type);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("simplefs");
MODULE_DESCRIPTION("Minimal SimpleFS for Linux 6.12");
