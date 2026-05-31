#ifndef _SIMPLEFS_H
#define _SIMPLEFS_H

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x53465331U /* "SFS1" */
#define SIMPLEFS_VERSION 1U
#define SIMPLEFS_SECTOR_SIZE 512U
#define SIMPLEFS_NAME_MAX 128U
#define SIMPLEFS_FIRST_FILE_INO 2ULL

#define SIMPLEFS_IOC_MAGIC 's'

struct simplefs_disk_super {
  __le32 magic;
  __le32 version;
  __le64 total_sectors;

  __le64 super1_sector;
  __le64 super2_sector;

  __le64 meta_start_sector;
  __le32 meta_sector_count;

  __le64 data_start_sector;
  __le32 max_filename_len;
  __le32 max_file_sectors;
  __le32 file_count;

  __le32 checksum;
} __packed;

struct simplefs_disk_file_meta {
  __le64 size;
  __le32 hash;
  __le32 reserved;
} __packed;

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
  __u64 meta_start_sector;
  __u64 super1_sector;
  __u64 super2_sector;

  __u32 meta_sector_count;
  __u32 max_filename_len;
  __u32 max_file_sectors;
  __u32 file_count;

  bool erased;

  struct simplefs_file_meta *files;
  struct mutex lock;
};

struct simplefs_hashes_hdr {
  __u32 capacity;
  __u32 count;
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
  _IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_hashes_hdr)
#define SIMPLEFS_IOCTL_GET_MAP                                                 \
  _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_map_req)

extern char *disk_name;
extern unsigned long sb1_sector;
extern unsigned long sb2_sector;
extern unsigned int max_filename_len;
extern unsigned int max_file_sectors;

__u32 simplefs_calc_super_checksum(const struct simplefs_disk_super *ds);
bool simplefs_disk_super_valid(const struct simplefs_disk_super *ds);
int simplefs_read_disk_super(struct super_block *sb, __u64 sector,
                             struct simplefs_disk_super *ds);
int simplefs_write_disk_super(struct super_block *sb, __u64 sector,
                              const struct simplefs_disk_super *ds);
ssize_t simplefs_rw_bytes(struct super_block *sb, __u64 start_sector,
                          loff_t pos, void *buf, size_t len, bool write);
__u32 simplefs_hash_file(struct super_block *sb,
                         const struct simplefs_file_meta *fm);
int simplefs_zero_sector_range(struct super_block *sb, __u64 start_sector,
                               __u64 sector_count);

void simplefs_free_sb_info(struct simplefs_sb_info *sbi);
int simplefs_prepare_file_table(struct super_block *sb,
                                struct simplefs_sb_info *sbi);
int simplefs_find_file_index_by_name(struct simplefs_sb_info *sbi,
                                     const char *name, size_t len);
int simplefs_load_or_format(struct super_block *sb,
                            struct simplefs_sb_info *sbi);
int simplefs_fill_super(struct super_block *sb, void *data, int silent);

struct simplefs_file_meta *simplefs_meta_from_inode(struct inode *inode);
struct inode *simplefs_make_root_inode(struct super_block *sb);
struct inode *simplefs_make_reg_inode(struct super_block *sb, u32 index);
extern const struct inode_operations simplefs_dir_inode_ops;

extern const struct file_operations simplefs_file_ops;
extern const struct file_operations simplefs_dir_ops;

long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#endif
