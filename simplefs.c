// simplefs.c
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x53465331U /* "SFS1" */
#define SIMPLEFS_VERSION 1U
#define SIMPLEFS_SECTOR_SIZE 512U
#define SIMPLEFS_NAME_MAX 128U
#define SIMPLEFS_FIRST_FILE_INO 2ULL

#define SIMPLEFS_IOC_MAGIC 's'

struct simplefs_hashes_req {
  __u32 capacity; /* in hashes[] entries */
  __u32 count;    /* returned number of hashes */
  __u32 hashes[]; /* flexible array */
};

struct simplefs_map_req {
  char name[SIMPLEFS_NAME_MAX];
  __u64 start_sector;
  __u32 sector_count;
  __u32 hash;
  __u64 size;
};

#define SIMPLEFS_IOCTL_CLEAR _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOCTL_ERASE _IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOCTL_GET_HASHES                                              \
  _IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_hashes_req)
#define SIMPLEFS_IOCTL_GET_MAP                                                 \
  _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_map_req)

/* Module parameters */
static char *disk_name;
static unsigned long sb1_sector = 1;
static unsigned long sb2_sector = 8;
static unsigned int max_filename_len = 32;
static unsigned int max_file_sectors = 4;

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

/* On-disk superblock (stored in one sector, 2 copies) */
struct simplefs_disk_super {
  __u32 magic;
  __u32 version;
  __u64 total_sectors;

  __u64 super1_sector;
  __u64 super2_sector;

  __u64 data_start_sector;
  __u32 max_filename_len;
  __u32 max_file_sectors;
  __u32 file_count;

  __u32 checksum; /* crc32 over all previous bytes in this struct */
};

/* Per-file metadata (in memory for now) */
struct simplefs_file_meta {
  char *name;
  __u64 start_sector;
  __u32 sector_count;
  __u64 size;
  __u32 hash;
};

struct simplefs_sb_info {
  __u64 total_sectors;
  __u64 data_start_sector;
  __u64 super1_sector;
  __u64 super2_sector;

  __u32 max_filename_len;
  __u32 max_file_sectors;
  __u32 file_count;

  struct simplefs_file_meta *files;
  struct mutex lock;
};

static struct file_system_type simplefs_fs_type;

/* ---------- helpers ---------- */

static unsigned int simplefs_digits_u32(__u32 v) {
  unsigned int d = 1;

  while (v >= 10) {
    v /= 10;
    d++;
  }
  return d;
}

static __u32
simplefs_calc_super_checksum(const struct simplefs_disk_super *ds) {
  /* checksum field is the last field, so exclude it */
  return crc32(~0U, (const unsigned char *)ds,
               offsetof(struct simplefs_disk_super, checksum));
}

static bool simplefs_disk_super_valid(const struct simplefs_disk_super *ds) {
  if (ds->magic != SIMPLEFS_MAGIC)
    return false;
  if (ds->version != SIMPLEFS_VERSION)
    return false;
  return ds->checksum == simplefs_calc_super_checksum(ds);
}

static int simplefs_read_sector(struct super_block *sb, __u64 sector,
                                void *buf) {
  struct buffer_head *bh;

  bh = sb_bread(sb, sector);
  if (!bh)
    return -EIO;

  memcpy(buf, bh->b_data, SIMPLEFS_SECTOR_SIZE);
  brelse(bh);
  return 0;
}

static int simplefs_write_sector(struct super_block *sb, __u64 sector,
                                 const void *buf) {
  struct buffer_head *bh;

  bh = sb_bread(sb, sector);
  if (!bh)
    return -EIO;

  memcpy(bh->b_data, buf, SIMPLEFS_SECTOR_SIZE);
  mark_buffer_dirty(bh);
  sync_dirty_buffer(bh);
  brelse(bh);
  return 0;
}

static int simplefs_read_disk_super(struct super_block *sb, __u64 sector,
                                    struct simplefs_disk_super *ds) {
  unsigned char buf[SIMPLEFS_SECTOR_SIZE];
  int ret;

  ret = simplefs_read_sector(sb, sector, buf);
  if (ret)
    return ret;

  memset(ds, 0, sizeof(*ds));
  memcpy(ds, buf, sizeof(*ds));
  return 0;
}

static int simplefs_write_disk_super(struct super_block *sb, __u64 sector,
                                     const struct simplefs_disk_super *ds) {
  unsigned char buf[SIMPLEFS_SECTOR_SIZE];

  memset(buf, 0, sizeof(buf));
  memcpy(buf, ds, sizeof(*ds));
  return simplefs_write_sector(sb, sector, buf);
}

static int simplefs_rw_bytes(struct super_block *sb, __u64 start_sector,
                             loff_t pos, void *buf, size_t len, bool write) {
  size_t done = 0;

  while (done < len) {
    __u64 abs_pos = pos + done;
    __u64 sector = start_sector + (abs_pos / SIMPLEFS_SECTOR_SIZE);
    size_t off = abs_pos % SIMPLEFS_SECTOR_SIZE;
    size_t chunk = min_t(size_t, len - done, SIMPLEFS_SECTOR_SIZE - off);
    struct buffer_head *bh;

    bh = sb_bread(sb, sector);
    if (!bh)
      return done ? (ssize_t)done : -EIO;

    if (write) {
      memcpy(bh->b_data + off, (u8 *)buf + done, chunk);
      mark_buffer_dirty(bh);
      sync_dirty_buffer(bh);
    } else {
      memcpy((u8 *)buf + done, bh->b_data + off, chunk);
    }

    brelse(bh);
    done += chunk;
  }

  return (ssize_t)done;
}

static __u32 simplefs_hash_file(struct super_block *sb,
                                const struct simplefs_file_meta *fm) {
  __u32 h = ~0U;
  __u64 remaining = fm->size;
  __u64 pos = 0;
  u8 sector_buf[SIMPLEFS_SECTOR_SIZE];

  if (remaining == 0)
    return 0;

  while (remaining > 0) {
    size_t chunk = min_t(__u64, remaining, SIMPLEFS_SECTOR_SIZE);
    ssize_t ret =
        simplefs_rw_bytes(sb, fm->start_sector, pos, sector_buf, chunk, false);
    if (ret < 0)
      return 0;
    h = crc32(h, sector_buf, chunk);
    remaining -= chunk;
    pos += chunk;
  }

  return h;
}

static void simplefs_zero_sector_range(struct super_block *sb,
                                       __u64 start_sector, __u64 sector_count) {
  u8 zero[SIMPLEFS_SECTOR_SIZE] = {0};
  __u64 i;

  for (i = 0; i < sector_count; i++)
    (void)simplefs_write_sector(sb, start_sector + i, zero);
}

static void simplefs_free_sb_info(struct simplefs_sb_info *sbi) {
  u32 i;

  if (!sbi)
    return;

  if (sbi->files) {
    for (i = 0; i < sbi->file_count; i++)
      kfree(sbi->files[i].name);
    kfree(sbi->files);
  }

  kfree(sbi);
}

static int simplefs_format_name(char *dst, size_t dstlen, u32 idx,
                                u32 file_count) {
  unsigned int width = simplefs_digits_u32(file_count ? (file_count - 1) : 0);
  int n;

  if (dstlen == 0)
    return -EINVAL;

  n = snprintf(dst, dstlen, "file%0*u", width, idx);
  if (n < 0)
    return -EINVAL;
  if ((size_t)n >= dstlen)
    return -ENAMETOOLONG;

  return 0;
}

static int simplefs_prepare_file_table(struct super_block *sb,
                                       struct simplefs_sb_info *sbi) {
  u32 i;
  u32 need_len;

  if (sbi->file_count == 0)
    return -EINVAL;

  need_len = 4 + simplefs_digits_u32(sbi->file_count - 1) +
             1; /* "file" + digits + NUL */
  if (sbi->max_filename_len < need_len)
    return -ENAMETOOLONG;

  sbi->files = kcalloc(sbi->file_count, sizeof(*sbi->files), GFP_KERNEL);
  if (!sbi->files)
    return -ENOMEM;

  for (i = 0; i < sbi->file_count; i++) {
    int ret;

    sbi->files[i].name =
        kcalloc(sbi->max_filename_len, sizeof(char), GFP_KERNEL);
    if (!sbi->files[i].name)
      return -ENOMEM;

    ret = simplefs_format_name(sbi->files[i].name, sbi->max_filename_len, i,
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

static void simplefs_free_file_table(struct simplefs_sb_info *sbi) {
  u32 i;

  if (!sbi || !sbi->files)
    return;

  for (i = 0; i < sbi->file_count; i++)
    kfree(sbi->files[i].name);

  kfree(sbi->files);
  sbi->files = NULL;
}

static int simplefs_find_file_index_by_name(struct simplefs_sb_info *sbi,
                                            const char *name, size_t len) {
  u32 i;

  for (i = 0; i < sbi->file_count; i++) {
    if (strlen(sbi->files[i].name) == len &&
        !memcmp(sbi->files[i].name, name, len))
      return (int)i;
  }

  return -ENOENT;
}

static struct simplefs_file_meta *
simplefs_meta_from_inode(struct inode *inode) {
  struct super_block *sb = inode->i_sb;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  u64 ino = inode->i_ino;

  if (!sbi || ino < SIMPLEFS_FIRST_FILE_INO)
    return NULL;

  ino -= SIMPLEFS_FIRST_FILE_INO;
  if (ino >= sbi->file_count)
    return NULL;

  return &sbi->files[ino];
}

static struct inode *simplefs_make_dir_inode(struct super_block *sb,
                                             umode_t mode) {
  struct inode *inode = new_inode(sb);

  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
  inode->i_ino = 1;
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
  inode->i_op = NULL;
  inode->i_fop = NULL;

  return inode;
}

static struct inode *simplefs_make_reg_inode(struct super_block *sb,
                                             u32 index) {
  struct inode *inode = new_inode(sb);
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  struct simplefs_file_meta *fm;

  if (!inode)
    return NULL;

  fm = &sbi->files[index];

  inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFREG | 0644);
  inode->i_ino = SIMPLEFS_FIRST_FILE_INO + index;
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
  inode->i_size = fm->size;
  inode->i_blocks = fm->sector_count * (SIMPLEFS_SECTOR_SIZE / 512);
  inode->i_op = NULL;
  inode->i_fop = NULL;

  return inode;
}

/* ---------- IOCTL ---------- */

static long simplefs_ioctl(struct file *file, unsigned int cmd,
                           unsigned long arg) {
  struct inode *inode = file_inode(file);
  struct super_block *sb = inode->i_sb;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  void __user *argp = (void __user *)arg;
  int ret = 0;

  if (!sbi)
    return -EIO;

  mutex_lock(&sbi->lock);

  switch (cmd) {
  case SIMPLEFS_IOCTL_CLEAR: {
    u8 zero[SIMPLEFS_SECTOR_SIZE] = {0};
    u32 i;
    u32 s;

    for (i = 0; i < sbi->file_count; i++) {
      struct simplefs_file_meta *fm = &sbi->files[i];

      for (s = 0; s < fm->sector_count; s++)
        ret = simplefs_write_sector(sb, fm->start_sector + s, zero);
      if (ret)
        break;

      fm->size = 0;
      fm->hash = 0;
    }
    break;
  }

  case SIMPLEFS_IOCTL_ERASE: {
    u8 zero[SIMPLEFS_SECTOR_SIZE] = {0};
    u32 i;
    u32 s;

    /* clear all file data */
    for (i = 0; i < sbi->file_count; i++) {
      struct simplefs_file_meta *fm = &sbi->files[i];

      for (s = 0; s < fm->sector_count; s++)
        ret = simplefs_write_sector(sb, fm->start_sector + s, zero);
      if (ret)
        break;

      fm->size = 0;
      fm->hash = 0;
    }

    if (!ret) {
      /* erase both superblock copies */
      ret = simplefs_write_sector(sb, sbi->super1_sector, zero);
      if (!ret)
        ret = simplefs_write_sector(sb, sbi->super2_sector, zero);
    }
    break;
  }

  case SIMPLEFS_IOCTL_GET_HASHES: {
    struct {
      __u32 capacity;
      __u32 count;
    } hdr;
    __u32 *hashes = NULL;
    __u32 n, i;
    size_t bytes;

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

  mutex_unlock(&sbi->lock);
  return ret;
}

/* ---------- file ops ---------- */

static ssize_t simplefs_read(struct file *file, char __user *buf, size_t len,
                             loff_t *ppos) {
  struct inode *inode = file_inode(file);
  struct simplefs_file_meta *fm = simplefs_meta_from_inode(inode);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  ssize_t ret;

  if (!fm)
    return -EIO;

  mutex_lock(&sbi->lock);

  if (*ppos >= fm->size) {
    mutex_unlock(&sbi->lock);
    return 0;
  }

  len = min_t(size_t, len, (size_t)(fm->size - *ppos));
  ret = simplefs_rw_bytes(inode->i_sb, fm->start_sector, *ppos, (void *)buf,
                          len, false);
  if (ret > 0)
    *ppos += ret;

  mutex_unlock(&sbi->lock);
  return ret;
}

static ssize_t simplefs_write(struct file *file, const char __user *buf,
                              size_t len, loff_t *ppos) {
  struct inode *inode = file_inode(file);
  struct simplefs_file_meta *fm = simplefs_meta_from_inode(inode);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  loff_t cap;
  ssize_t ret;

  if (!fm)
    return -EIO;

  mutex_lock(&sbi->lock);

  cap = (loff_t)fm->sector_count * SIMPLEFS_SECTOR_SIZE;
  if (*ppos >= cap) {
    mutex_unlock(&sbi->lock);
    return -ENOSPC;
  }

  if (*ppos + len > cap)
    len = (size_t)(cap - *ppos);

  ret = simplefs_rw_bytes(inode->i_sb, fm->start_sector, *ppos, (void *)buf,
                          len, true);
  if (ret > 0) {
    loff_t end = *ppos + ret;

    *ppos = end;
    if (end > fm->size)
      fm->size = end;

    fm->hash = simplefs_hash_file(inode->i_sb, fm);
    i_size_write(inode, fm->size);
  }

  mutex_unlock(&sbi->lock);
  return ret;
}

static const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .read = simplefs_read,
    .write = simplefs_write,
    .unlocked_ioctl = simplefs_ioctl,
    .llseek = default_llseek,
};

static int simplefs_iterate(struct file *file, struct dir_context *ctx) {
  struct inode *inode = file_inode(file);
  struct simplefs_sb_info *sbi = inode->i_sb->s_fs_info;
  loff_t i;

  if (!dir_emit_dots(file, ctx))
    return 0;

  if (ctx->pos < 2)
    i = 0;
  else
    i = ctx->pos - 2;

  for (; i < sbi->file_count; i++) {
    const char *name = sbi->files[i].name;
    unsigned int nlen = strlen(name);

    if (!dir_emit(ctx, name, nlen, SIMPLEFS_FIRST_FILE_INO + i, DT_REG))
      return 0;

    ctx->pos = i + 3; /* dots (0,1), files start at 2 */
  }

  return 0;
}

static const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
    .unlocked_ioctl = simplefs_ioctl,
    .llseek = generic_file_llseek,
};

/* ---------- inode ops ---------- */

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags) {
  struct super_block *sb = dir->i_sb;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  int idx;
  struct inode *inode = NULL;

  if (!sbi)
    return ERR_PTR(-EIO);

  idx = simplefs_find_file_index_by_name(sbi, dentry->d_name.name,
                                         dentry->d_name.len);
  if (idx >= 0) {
    inode = simplefs_make_reg_inode(sb, (u32)idx);
    if (!inode)
      return ERR_PTR(-ENOMEM);
    inode->i_fop = &simplefs_file_ops;
  }

  d_add(dentry, inode);
  return NULL;
}

static const struct inode_operations simplefs_dir_inode_ops = {
    .lookup = simplefs_lookup,
};

static struct inode *simplefs_make_root_inode(struct super_block *sb) {
  struct inode *inode = simplefs_make_dir_inode(sb, S_IFDIR | 0755);

  if (!inode)
    return NULL;

  inode->i_op = &simplefs_dir_inode_ops;
  inode->i_fop = &simplefs_dir_ops;
  return inode;
}

/* ---------- superblock / mount ---------- */

static int simplefs_load_or_format(struct super_block *sb,
                                   struct simplefs_sb_info *sbi) {
  struct simplefs_disk_super ds1, ds2, *chosen = NULL;
  struct simplefs_disk_super newds;
  __u64 dev_sectors;
  u64 file_bytes;
  int ret;

  dev_sectors = i_size_read(sb->s_bdev->bd_inode) / SIMPLEFS_SECTOR_SIZE;
  sbi->total_sectors = dev_sectors;

  if (sb1_sector == sb2_sector)
    return -EINVAL;

  if (sb1_sector >= dev_sectors || sb2_sector >= dev_sectors)
    return -EINVAL;

  ret = simplefs_read_disk_super(sb, sbi->super1_sector, &ds1);
  if (!ret && simplefs_disk_super_valid(&ds1))
    chosen = &ds1;

  ret = simplefs_read_disk_super(sb, sbi->super2_sector, &ds2);
  if (!ret && simplefs_disk_super_valid(&ds2))
    chosen = chosen ? chosen : &ds2;

  if (chosen) {
    if (chosen->total_sectors > dev_sectors)
      return -EINVAL;

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

  /* format fresh FS */
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

  /* zero file area */
  for (u32 i = 0; i < sbi->file_count; i++) {
    struct simplefs_file_meta *fm = &sbi->files[i];
    simplefs_zero_sector_range(sb, fm->start_sector, fm->sector_count);
  }

  return 0;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent) {
  struct simplefs_sb_info *sbi;
  struct inode *root_inode;
  int ret;

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
  sb->s_maxbytes = (loff_t)max_file_sectors * SIMPLEFS_SECTOR_SIZE;

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
  simplefs_free_file_table(sbi);
  sb->s_fs_info = NULL;
  kfree(sbi);
  return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data) {
  if (disk_name && strcmp(disk_name, dev_name) != 0) {
    pr_err("expected disk_name=%s, got %s\n", disk_name, dev_name);
    return ERR_PTR(-EINVAL);
  }

  return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static void simplefs_kill_sb(struct super_block *sb) {
  struct simplefs_sb_info *sbi = sb->s_fs_info;

  kill_block_super(sb);
  simplefs_free_file_table(sbi);
  kfree(sbi);
}

static struct file_system_type simplefs_fs_type = {
    .owner = THIS_MODULE,
    .name = SIMPLEFS_NAME,
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

/* ---------- module init/exit ---------- */

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
MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("SimpleFS single-file skeleton for Linux 6.12");
