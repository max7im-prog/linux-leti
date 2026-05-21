#include "simplefs.h"

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>

char *disk_name;
unsigned long sb1_sector = 1;
unsigned long sb2_sector = 8;
unsigned int max_filename_len = 32;
unsigned int max_file_sectors = 4;

module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name,
                 "Block device name expected by this FS, e.g. /dev/loop0");

module_param(sb1_sector, ulong, 0444);
MODULE_PARM_DESC(sb1_sector, "Primary superblock sector");

module_param(sb2_sector, ulong, 0444);
MODULE_PARM_DESC(sb2_sector, "Backup superblock sector");

module_param(max_filename_len, uint, 0444);
MODULE_PARM_DESC(max_filename_len, "Maximum file name length");

module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors");

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data) {
  if (disk_name && dev_name && strcmp(disk_name, dev_name) != 0) {
    pr_err("expected disk_name=%s, got %s\n", disk_name, dev_name);
    return ERR_PTR(-EINVAL);
  }

  return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static void simplefs_kill_sb(struct super_block *sb) {
  struct simplefs_sb_info *sbi = sb->s_fs_info;

  kill_block_super(sb);
  simplefs_free_sb_info(sbi);
}

static struct file_system_type simplefs_fs_type = {
    .owner = THIS_MODULE,
    .name = SIMPLEFS_NAME,
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void) {
  int ret;

  ret = register_filesystem(&simplefs_fs_type);
  if (ret) {
    pr_err("register_filesystem failed: %d\n", ret);
    return ret;
  }

  pr_info("loaded\n");
  return 0;
}

static void __exit simplefs_exit(void) {
  unregister_filesystem(&simplefs_fs_type);
  pr_info("unloaded\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("max7im");
MODULE_DESCRIPTION("SimpleFS module for Linux 6.12");
