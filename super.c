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
  if (!sbi || sbi->erased || !sbi->files)
    return -ENOENT;
  u32 i;

  for (i = 0; i < sbi->file_count; i++) {
    if (strlen(sbi->files[i].name) == len &&
        !memcmp(sbi->files[i].name, name, len))
      return (int)i;
  }

  return -ENOENT;
}

static bool simplefs_disk_super_is_zero(const struct simplefs_disk_super *ds) {
  static const struct simplefs_disk_super zero;

  return !memcmp(ds, &zero, sizeof(*ds));
}

static int
simplefs_validate_loaded_super(struct super_block *sb,
                               const struct simplefs_disk_super *ds) {
  __u64 dev_sectors = bdev_nr_bytes(sb->s_bdev) >> 9;

  if (ds->magic != SIMPLEFS_MAGIC)
    return -EUCLEAN;

  if (ds->version != SIMPLEFS_VERSION)
    return -EUCLEAN;

  if (ds->checksum != simplefs_calc_super_checksum(ds))
    return -EUCLEAN;

  if (ds->total_sectors != dev_sectors)
    return -EUCLEAN;

  if (ds->super1_sector >= dev_sectors || ds->super2_sector >= dev_sectors ||
      ds->super1_sector == ds->super2_sector)
    return -EINVAL;

  if (ds->max_filename_len == 0 || ds->max_file_sectors == 0 ||
      ds->file_count == 0)
    return -EINVAL;

  if (ds->data_start_sector >= dev_sectors)
    return -EINVAL;

  if (ds->data_start_sector + (u64)ds->file_count * ds->max_file_sectors >
      ds->total_sectors)
    return -EINVAL;

  return 0;
}

int simplefs_load_or_format(struct super_block *sb,
                            struct simplefs_sb_info *sbi) {
  struct simplefs_disk_super ds1, ds2, newds;
  struct simplefs_disk_super *chosen = NULL;
  __u64 dev_bytes, dev_sectors;
  u64 available_sectors;
  int ret1, ret2;
  int ret;
  bool ds1_read_ok, ds2_read_ok;
  bool ds1_zero = false, ds2_zero = false;
  bool ds1_valid = false, ds2_valid = false;

  dev_bytes = bdev_nr_bytes(sb->s_bdev);
  dev_sectors = dev_bytes >> 9; /* 512-byte sectors */
  sbi->total_sectors = dev_sectors;

  if (sbi->super1_sector == sbi->super2_sector)
    return -EINVAL;

  if (sbi->super1_sector >= dev_sectors || sbi->super2_sector >= dev_sectors)
    return -EINVAL;

  ret1 = simplefs_read_disk_super(sb, sbi->super1_sector, &ds1);
  ret2 = simplefs_read_disk_super(sb, sbi->super2_sector, &ds2);

  ds1_read_ok = (ret1 == 0);
  ds2_read_ok = (ret2 == 0);

  if (ds1_read_ok) {
    ds1_zero = simplefs_disk_super_is_zero(&ds1);
    ds1_valid = !ds1_zero && (simplefs_validate_loaded_super(sb, &ds1) == 0);
  }

  if (ds2_read_ok) {
    ds2_zero = simplefs_disk_super_is_zero(&ds2);
    ds2_valid = !ds2_zero && (simplefs_validate_loaded_super(sb, &ds2) == 0);
  }

  /*
   * If at least one copy is valid, mount from it and try to restore
   * the other copy if needed.
   */
  if (ds1_valid && ds2_valid) {
    chosen = &ds1;

    /* Keep copies in sync if they differ. */
    if (memcmp(&ds1, &ds2, sizeof(ds1)) != 0) {
      ret = simplefs_write_disk_super(sb, sbi->super2_sector, &ds1);
      if (ret)
        pr_warn("simplefs: failed to resync super2: %d\n", ret);
    }
  } else if (ds1_valid) {
    chosen = &ds1;

    /* Restore second copy if possible. */
    ret = simplefs_write_disk_super(sb, sbi->super2_sector, &ds1);
    if (ret)
      pr_warn("simplefs: failed to restore super2: %d\n", ret);
  } else if (ds2_valid) {
    chosen = &ds2;

    /* Restore first copy if possible. */
    ret = simplefs_write_disk_super(sb, sbi->super1_sector, &ds2);
    if (ret)
      pr_warn("simplefs: failed to restore super1: %d\n", ret);
  } else if ((ds1_read_ok && ds1_zero) && (ds2_read_ok && ds2_zero)) {
    /* Fresh device: both copies are empty. */
    chosen = NULL;
  } else {
    /*
     * Both copies are invalid, or one copy couldn't be read and the
     * other one is not valid enough to mount.
     * Do not format silently.
     */
    return -EUCLEAN;
  }

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
  available_sectors = (dev_sectors > newds.data_start_sector)
                          ? (dev_sectors - newds.data_start_sector)
                          : 0;

  newds.file_count = (u32)(available_sectors / sbi->max_file_sectors);
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

  return 0;
}

int simplefs_fill_super(struct super_block *sb, void *data, int silent) {
  struct simplefs_sb_info *sbi;
  struct inode *root_inode;
  int ret;

  (void)data;
  (void)silent;

  pr_info("befor kzalloc\n");
  sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
  pr_info("after kzalloc\n");
  if (!sbi)
    return -ENOMEM;

  pr_info("before mutex_init\n");
  mutex_init(&sbi->lock);
  pr_info("after mutex_init\n");

  sbi->super1_sector = sb1_sector;
  sbi->super2_sector = sb2_sector;
  sbi->max_filename_len = min_t(u32, max_filename_len, SIMPLEFS_NAME_MAX);
  sbi->max_file_sectors = max_file_sectors;

  if (sbi->max_filename_len < 8 || sbi->max_file_sectors == 0) {
    kfree(sbi);
    return -EINVAL;
  }

  sb->s_magic = SIMPLEFS_MAGIC;

  if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE)) {
    ret = -EINVAL;
    goto fail;
  }

  sb->s_fs_info = sbi;
  sb->s_op = &simplefs_super_ops;
  sb->s_maxbytes = (loff_t)sbi->max_file_sectors * SIMPLEFS_SECTOR_SIZE;

  pr_info("before load_or_format\n");
  ret = simplefs_load_or_format(sb, sbi);
  pr_info("after load_or_format\n");
  if (ret)
    goto fail;

  pr_info("before make_root_inode\n");
  root_inode = simplefs_make_root_inode(sb);
  pr_info("after make_root_inode\n");
  if (!root_inode) {
    ret = -ENOMEM;
    goto fail;
  }

  pr_info("before d_make_root\n");
  sb->s_root = d_make_root(root_inode);
  pr_info("after d_make_root\n");
  if (!sb->s_root) {
    ret = -ENOMEM;
    goto fail;
  }

  sbi->erased = false;

  pr_info("mounted: files=%u data_start=%llu\n", sbi->file_count,
          sbi->data_start_sector);

  return 0;

fail:
  pr_info("fail - before free_sb_info\n");
  simplefs_free_sb_info(sbi);
  pr_info("fail - after free_sb_info\n");
  sb->s_fs_info = NULL;
  return ret;
}
