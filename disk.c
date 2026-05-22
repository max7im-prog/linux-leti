#include "simplefs.h"

#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/string.h>

__u32 simplefs_calc_super_checksum(const struct simplefs_disk_super *ds) {
  return crc32(~0U, (const unsigned char *)ds,
               offsetof(struct simplefs_disk_super, checksum));
}

bool simplefs_disk_super_valid(const struct simplefs_disk_super *ds) {
  if (ds->magic != SIMPLEFS_MAGIC)
    return false;
  if (ds->version != SIMPLEFS_VERSION)
    return false;
  return ds->checksum == simplefs_calc_super_checksum(ds);
}

static int simplefs_read_sector(struct super_block *sb, __u64 sector,
                                void *buf) {
  struct buffer_head *bh;

  pr_info("read_sector: sector=%llu\n", (unsigned long long)sector);

  bh = sb_bread(sb, sector);

  pr_info("read_sector: after sb_bread\n");
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

int simplefs_read_disk_super(struct super_block *sb, __u64 sector,
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

int simplefs_write_disk_super(struct super_block *sb, __u64 sector,
                              const struct simplefs_disk_super *ds) {
  unsigned char buf[SIMPLEFS_SECTOR_SIZE];

  memset(buf, 0, sizeof(buf));
  memcpy(buf, ds, sizeof(*ds));
  return simplefs_write_sector(sb, sector, buf);
}

ssize_t simplefs_rw_bytes(struct super_block *sb, __u64 start_sector,
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

__u32 simplefs_hash_file(struct super_block *sb,
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

int simplefs_zero_sector_range(struct super_block *sb, __u64 start_sector,
                               __u64 sector_count) {
  u8 zero[SIMPLEFS_SECTOR_SIZE] = {0};
  __u64 i;
  int ret = 0;

  for (i = 0; i < sector_count; i++) {
    ret = simplefs_write_sector(sb, start_sector + i, zero);
    if (ret)
      return ret;
  }

  return 0;
}
