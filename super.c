#include "simplefs.h"

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>

static const struct super_operations simplefs_super_ops = {};

static int simplefs_format_name(char *dst, size_t dstlen, u32 idx,
                                u32 file_count) {
  unsigned int width = 1;
  u32 n = (file_count > 0) ? (file_count - 1) : 0;
  int ret;

  while (n >= 10) {
    n /= 10;
    width++;
  }

  ret = snprintf(dst, dstlen, "file%0*u", width, idx);
  if (ret < 0)
    return -EINVAL;
  if ((size_t)ret >= dstlen)
    return -ENAMETOOLONG;

  return 0;
}

void simplefs_free_sb_info(struct simplefs_sb_info *sbi) {
  u32 i;

  if (!sbi)
    return;

  if (sbi->files) {
    for (i = 0; i < sbi->file_count; i++)
      kfree(sbi->files[i].name);
    kfree(sbi->files);
    sbi->files = NULL;
  }

  kfree(sbi);
}

int simplefs_prepare_file_table(struct super_block *sb,
                                struct simplefs_sb_info *sbi) {
  u32 i;
  u32 need_len;

  (void)sb;

  if (sbi->file_count == 0)
    return -EINVAL;

  need_len = 4 + 1; /* "file" + at least one digit */
  if (sbi->file_count > 1)
    need_len += 1;

  if (sbi->max_filename_len < need_len)
    return -ENAMETOOLONG;

  sbi->files = kcalloc(sbi->file_count, sizeof(*sbi->files), GFP_KERNEL);
  if (!sbi->files)
    return -ENOMEM;

  for (i = 0; i < sbi->file_count; i++) {
    int ret;

    sbi->files[i].name = kzalloc(sbi->max_filename_len + 1, GFP_KERNEL);
    if (!sbi->files[i].name)
      return -ENOMEM;

    ret = simplefs_format_name(sbi->files[i].name, sbi->max_filename_len + 1, i,
                               sbi->file_count);
    if (ret)
      return ret;

    sbi->files[i].start_sector =
        sbi->data_start_sector + ((u64)i * sbi->max_file_sectors);
    sbi->files[i].sector_count = sbi->max_file_sectors;
    sbi->files[i].size = 0;
    sbi->files[i].hash = 0;
  }

  return 0;
}

int simplefs_find_file_index_by_name(struct simplefs_sb_info *sbi,
                                     const char *name, size_t len) {
  u32 i;

  for (i = 0; i < sbi->file_count; i++) {
    if (strlen(sbi->files[i].name) == len &&
        !memcmp(sbi->files[i].name, name, len))
      return (int)i;
  }

  return -ENOENT;
}

int simplefs_load_or_format(struct super_block *sb,
                            struct simplefs_sb_info *sbi) {
  struct simplefs_disk_super ds1, ds2, *chosen = NULL;
  struct simplefs_disk_super newds;
  __u64 dev_sectors;
  __u64 dev_bytes;
  u64 file_bytes;
  int ret;

  dev_bytes = bdev_nr_bytes(sb->s_bdev);
  dev_sectors = dev_bytes >> 9;
  sbi->total_sectors = dev_sectors;

  if (sbi->super1_sector == sbi->super2_sector)
    return -EINVAL;

  if (sbi->super1_sector >= dev_sectors || sbi->super2_sector >= dev_sectors)
    return -EINVAL;

  ret = simplefs_read_disk_super(sb, sbi->super1_sector, &ds1);
  if (!ret && simplefs_disk_super_valid(&ds1))
    chosen = &ds1;

  ret = simplefs_read_disk_super(sb, sbi->super2_sector, &ds2);
  if (!ret && simplefs_disk_super_valid(&ds2))
    chosen = chosen ? chosen : &ds2;

  if (chosen) {
    sbi->total_sectors = chosen->total_sectors;
    sbi->data_start_sector = chosen->data_start_sector;
    sbi->max_filename_len =
        min_t(u32, chosen->max_filename_len, SIMPLEFS_NAME_MAX);
    sbi->max_file_sectors = chosen->max_file_sectors;
    sbi->file_count = chosen->file_count;

    if (sbi->max_filename_len == 0 || sbi->max_file_sectors == 0 ||
        sbi->file_count == 0)
      return -EINVAL;

    if (sbi->data_start_sector + (u64)sbi->file_count * sbi->max_file_sectors >
        sbi->total_sectors)
      return -EINVAL;

    ret = simplefs_prepare_file_table(sb, sbi);
    if (ret)
      return ret;

    return 0;
  }

  /* fresh format */
  memset(&newds, 0, sizeof(newds));
  newds.magic = SIMPLEFS_MAGIC;
  newds.version = SIMPLEFS_VERSION;
  newds.total_sectors = dev_sectors;
  newds.super1_sector = sbi->super1_sector;
  newds.super2_sector = sbi->super2_sector;
  newds.max_filename_len = sbi->max_filename_len;
  newds.max_file_sectors = sbi->max_file_sectors;

  newds.data_start_sector = max(sbi->super1_sector, sbi->super2_sector) + 1;
  file_bytes = dev_sectors > newds.data_start_sector
                   ? (dev_sectors - newds.data_start_sector)
                   : 0;
  newds.file_count = (u32)(file_bytes / sbi->max_file_sectors);

  if (newds.file_count == 0)
    return -EINVAL;

  sbi->data_start_sector = newds.data_start_sector;
  sbi->file_count = newds.file_count;

  ret = simplefs_prepare_file_table(sb, sbi);
  if (ret)
    return ret;

  newds.checksum = simplefs_calc_super_checksum(&newds);

  ret = simplefs_write_disk_super(sb, sbi->super1_sector, &newds);
  if (ret)
    return ret;

  ret = simplefs_write_disk_super(sb, sbi->super2_sector, &newds);
  if (ret)
    return ret;

  /* Don't zero all data sectors here. Mount must stay fast. */

  return 0;
}

int simplefs_fill_super(struct super_block *sb, void *data, int silent) {
  struct simplefs_sb_info *sbi;
  struct inode *root_inode;
  int ret;

  (void)data;
  (void)silent;

  sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
  if (!sbi)
    return -ENOMEM;

  mutex_init(&sbi->lock);

  sbi->super1_sector = sb1_sector;
  sbi->super2_sector = sb2_sector;
  sbi->max_filename_len = min_t(u32, max_filename_len, SIMPLEFS_NAME_MAX);
  sbi->max_file_sectors = max_file_sectors;

  if (sbi->max_filename_len < 8 || sbi->max_file_sectors == 0) {
    kfree(sbi);
    return -EINVAL;
  }

  sb->s_magic = SIMPLEFS_MAGIC;
  sb->s_blocksize = SIMPLEFS_SECTOR_SIZE;
  sb->s_blocksize_bits = 9;
  sb->s_fs_info = sbi;
  sb->s_op = &simplefs_super_ops;
  sb->s_maxbytes = (loff_t)sbi->max_file_sectors * SIMPLEFS_SECTOR_SIZE;

  ret = simplefs_load_or_format(sb, sbi);
  if (ret)
    goto fail;

  root_inode = simplefs_make_root_inode(sb);
  if (!root_inode) {
    ret = -ENOMEM;
    goto fail;
  }

  sb->s_root = d_make_root(root_inode);
  if (!sb->s_root) {
    ret = -ENOMEM;
    goto fail;
  }

  pr_info("mounted: files=%u data_start=%llu\n", sbi->file_count,
          sbi->data_start_sector);

  return 0;

fail:
  simplefs_free_sb_info(sbi);
  sb->s_fs_info = NULL;
  return ret;
}
