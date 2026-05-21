#include "simplefs.h"

#include <linux/fs.h>
#include <linux/slab.h>

struct simplefs_file_meta *simplefs_meta_from_inode(struct inode *inode) {
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

static const struct inode_operations simplefs_file_inode_ops = {};

static struct inode *simplefs_make_inode_common(struct super_block *sb,
                                                umode_t mode) {
  struct inode *inode = new_inode(sb);

  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
  return inode;
}

struct inode *simplefs_make_reg_inode(struct super_block *sb, u32 index) {
  struct inode *inode;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  struct simplefs_file_meta *fm;

  inode = simplefs_make_inode_common(sb, S_IFREG | 0644);
  if (!inode)
    return NULL;

  fm = &sbi->files[index];

  inode->i_ino = SIMPLEFS_FIRST_FILE_INO + index;
  inode->i_size = fm->size;
  inode->i_blocks = fm->sector_count * (SIMPLEFS_SECTOR_SIZE / 512);
  inode->i_op = &simplefs_file_inode_ops;
  inode->i_fop = &simplefs_file_ops;
  inode->i_nlink = 1;

  return inode;
}

static struct inode *simplefs_make_dir_inode(struct super_block *sb) {
  struct inode *inode;

  inode = simplefs_make_inode_common(sb, S_IFDIR | 0755);
  if (!inode)
    return NULL;

  inode->i_ino = 1;
  inode->i_op = &simplefs_dir_inode_ops;
  inode->i_fop = &simplefs_dir_ops;
  inode->i_nlink = 2;

  return inode;
}

struct inode *simplefs_make_root_inode(struct super_block *sb) {
  return simplefs_make_dir_inode(sb);
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags) {
  struct super_block *sb = dir->i_sb;
  struct simplefs_sb_info *sbi = sb->s_fs_info;
  int idx = simplefs_find_file_index_by_name(sbi, dentry->d_name.name,
                                             dentry->d_name.len);
  struct inode *inode = NULL;

  (void)flags;

  if (idx >= 0) {
    inode = simplefs_make_reg_inode(sb, (u32)idx);
    if (!inode)
      return ERR_PTR(-ENOMEM);
  }

  d_add(dentry, inode);
  return NULL;
}

const struct inode_operations simplefs_dir_inode_ops = {
    .lookup = simplefs_lookup,
};
