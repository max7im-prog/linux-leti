#include "simplefs.h"

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
                             loff_t *ppos) {

  struct inode *inode = file_inode(file);
  struct simplefs_file_meta *fm = simplefs_meta_from_inode(inode);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  void *kbuf;
  ssize_t ret;

  if (sbi->erased)
    return -EIO;

  if (!fm)
    return -EIO;

  if (len == 0)
    return 0;

  mutex_lock(&sbi->lock);

  if (*ppos >= fm->size) {
    mutex_unlock(&sbi->lock);
    return 0;
  }

  len = min_t(size_t, len, (size_t)(fm->size - *ppos));

  kbuf = kvmalloc(len, GFP_KERNEL);
  if (!kbuf) {
    mutex_unlock(&sbi->lock);
    return -ENOMEM;
  }

  ret =
      simplefs_rw_bytes(inode->i_sb, fm->start_sector, *ppos, kbuf, len, false);
  if (ret > 0) {
    if (copy_to_user(buf, kbuf, ret))
      ret = -EFAULT;
    else
      *ppos += ret;
  }

  kvfree(kbuf);
  mutex_unlock(&sbi->lock);
  return ret;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
                              size_t len, loff_t *ppos) {

  struct inode *inode = file_inode(file);
  struct simplefs_file_meta *fm = simplefs_meta_from_inode(inode);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  loff_t cap;
  void *kbuf;
  ssize_t ret;

  if (sbi->erased)
    return -EIO;

  if (!fm)
    return -EIO;

  if (len == 0)
    return 0;

  mutex_lock(&sbi->lock);

  cap = (loff_t)fm->sector_count * SIMPLEFS_SECTOR_SIZE;
  if (*ppos >= cap) {
    mutex_unlock(&sbi->lock);
    return -ENOSPC;
  }

  if (*ppos + len > cap)
    len = (size_t)(cap - *ppos);

  kbuf = kvmalloc(len, GFP_KERNEL);
  if (!kbuf) {
    mutex_unlock(&sbi->lock);
    return -ENOMEM;
  }

  if (copy_from_user(kbuf, buf, len)) {
    kvfree(kbuf);
    mutex_unlock(&sbi->lock);
    return -EFAULT;
  }

  ret =
      simplefs_rw_bytes(inode->i_sb, fm->start_sector, *ppos, kbuf, len, true);
  if (ret > 0) {
    loff_t end = *ppos + ret;

    *ppos = end;
    if (end > fm->size)
      fm->size = end;

    fm->hash = simplefs_hash_file(inode->i_sb, fm);
    i_size_write(inode, fm->size);
  }

  kvfree(kbuf);
  mutex_unlock(&sbi->lock);
  return ret;
}

static int simplefs_iterate(struct file *file, struct dir_context *ctx) {

  struct inode *inode = file_inode(file);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  loff_t i;

  if (sbi->erased || !sbi->files)
    return 0;

  if (!dir_emit_dots(file, ctx))
    return 0;

  if (ctx->pos <= 2)
    i = 0;
  else
    i = ctx->pos - 2;

  for (; i < sbi->file_count; i++) {
    const char *name = sbi->files[i].name;
    unsigned int nlen = strlen(name);

    if (!dir_emit(ctx, name, nlen, SIMPLEFS_FIRST_FILE_INO + i, DT_REG))
      return 0;

    ctx->pos = i + 3;
  }

  return 0;
}

const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .read = simplefs_read,
    .write = simplefs_write,
    .unlocked_ioctl = simplefs_ioctl,
    .llseek = default_llseek,
};

const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
    .unlocked_ioctl = simplefs_ioctl,
    .llseek = generic_file_llseek,
};
