#include "simplefs.h"

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static void simplefs_free_file_table(struct simplefs_sb_info *sbi) {
  u32 i;

  if (!sbi || !sbi->files)
    return;

  for (i = 0; i < sbi->file_count; i++)
    kfree(sbi->files[i].name);

  kfree(sbi->files);
  sbi->files = NULL;
  sbi->file_count = 0;
}

static int simplefs_erase_fs(struct super_block *sb,
                             struct simplefs_sb_info *sbi) {
  u32 i;
  int ret;

  if (!sbi->files || sbi->file_count == 0)
    return 0;

  for (i = 0; i < sbi->file_count; i++) {
    struct simplefs_file_meta *fm = &sbi->files[i];

    ret = simplefs_zero_sector_range(sb, fm->start_sector, fm->sector_count);
    if (ret)
      return ret;

    fm->size = 0;
    fm->hash = 0;
  }

  /* wipe both superblock copies */
  ret = simplefs_zero_sector_range(sb, sbi->super1_sector, 1);
  if (ret)
    return ret;

  ret = simplefs_zero_sector_range(sb, sbi->super2_sector, 1);
  if (ret)
    return ret;

  /* flush to disk */
  sync_blockdev(sb->s_bdev);

  /* destroy in-memory file table */
  simplefs_free_file_table(sbi);

  sbi->data_start_sector = 0;
  sbi->total_sectors = 0;
  sbi->erased = true;

  return 0;
}

long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  struct inode *inode = file_inode(file);
  struct super_block *sb = inode->i_sb;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  void __user *argp = (void __user *)arg;
  int ret = 0;

  pr_info("SIMPLEFS IOCTL CALLED: cmd=%x\n", cmd);

  if (!sbi)
    return -EIO;

  mutex_lock(&sbi->lock);

  /* after ERASE, filesystem is dead until remount/format */
  if (sbi->erased && cmd != SIMPLEFS_IOCTL_ERASE) {
    ret = -EIO;
    goto out;
  }

  switch (cmd) {
  case SIMPLEFS_IOCTL_CLEAR: {
    u8 zero[SIMPLEFS_SECTOR_SIZE] = {0};
    u32 i, s;

    if (!sbi->files || sbi->file_count == 0) {
      ret = -EIO;
      break;
    }

    for (i = 0; i < sbi->file_count; i++) {
      struct simplefs_file_meta *fm = &sbi->files[i];

      for (s = 0; s < fm->sector_count; s++) {
        ret = simplefs_rw_bytes(sb, fm->start_sector + s, 0, zero,
                                SIMPLEFS_SECTOR_SIZE, true);
        if (ret < 0)
          break;
      }

      fm->size = 0;
      fm->hash = 0;
      simplefs_store_file_meta(sb, i, fm);
    }
    sync_blockdev(sb->s_bdev);
    break;
  }

  case SIMPLEFS_IOCTL_ERASE:
    ret = simplefs_erase_fs(sb, sbi);
    break;

  case SIMPLEFS_IOCTL_GET_HASHES: {
    struct simplefs_hashes_hdr hdr;
    __u32 n, i;
    __u32 *hashes = NULL;
    size_t bytes;

    if (!sbi->files || sbi->file_count == 0) {
      ret = -EIO;
      break;
    }

    if (copy_from_user(&hdr, argp, sizeof(hdr))) {
      ret = -EFAULT;
      break;
    }

    n = min(hdr.capacity, sbi->file_count);
    hdr.count = n;
    bytes = (size_t)n * sizeof(__u32);

    hashes = kcalloc(n ? n : 1, sizeof(__u32), GFP_KERNEL);
    if (!hashes) {
      ret = -ENOMEM;
      break;
    }

    for (i = 0; i < n; i++)
      hashes[i] = sbi->files[i].hash;

    if (copy_to_user(argp, &hdr, sizeof(hdr))) {
      ret = -EFAULT;
      kfree(hashes);
      break;
    }

    if (n && copy_to_user((char __user *)argp + sizeof(hdr), hashes, bytes))
      ret = -EFAULT;

    kfree(hashes);
    break;
  }

  case SIMPLEFS_IOCTL_GET_MAP: {
    struct simplefs_map_req req;
    int idx;

    if (!sbi->files || sbi->file_count == 0) {
      ret = -EIO;
      break;
    }

    memset(&req, 0, sizeof(req));
    if (copy_from_user(&req, argp, sizeof(req))) {
      ret = -EFAULT;
      break;
    }

    idx = simplefs_find_file_index_by_name(
        sbi, req.name, strnlen(req.name, SIMPLEFS_NAME_MAX));
    if (idx < 0) {
      ret = idx;
      break;
    }

    req.start_sector = sbi->files[idx].start_sector;
    req.sector_count = sbi->files[idx].sector_count;
    req.hash = sbi->files[idx].hash;
    req.size = sbi->files[idx].size;

    if (copy_to_user(argp, &req, sizeof(req)))
      ret = -EFAULT;

    break;
  }

  default:
    ret = -ENOTTY;
    break;
  }

out:
  mutex_unlock(&sbi->lock);
  return ret;
}
