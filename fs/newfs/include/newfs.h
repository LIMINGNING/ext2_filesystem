#ifndef _NEWFS_H_
#define _NEWFS_H_

#define FUSE_USE_VERSION 26
#include "stdio.h"
#include "stdlib.h"
#include <unistd.h>
#include "fcntl.h"
#include "string.h"
#include "fuse.h"
#include <stddef.h>
#include "ddriver.h"
#include "errno.h"
#include "types.h"
#include "stdint.h"

#define NEWFS_MAGIC                  /* TODO: Define by yourself */
#define NEWFS_DEFAULT_PERM    0777   /* 全权限打开 */

#define NFS_MAGIC_NUM 0x52415453
#define NFS_BLKS_SZ() (1024)
#define NFS_IO_SZ() (512)

/* 错误码 */
#define NFS_ERROR_NONE          0
#define NFS_ERROR_IO            1
#define NFS_ERROR_NOTFOUND      2
#define NFS_ERROR_NOSPACE       3
#define NFS_ERROR_INVAL         4
#define NFS_ERROR_EXISTS        5
#define NFS_ERROR_UNSUPPORTED   6

/* 偏移计算 */
#define NFS_SUPER_OFS           0
#define NFS_INO_OFS(ino)        (super.inode_offset * NFS_BLKS_SZ() + \
                                 (ino) * sizeof(struct newfs_inode_d))

/* 对齐宏 */
#define NFS_ROUND_DOWN(value, round) ((value) & (~((round) - 1)))
#define NFS_ROUND_UP(value, round)   (((value) + (round) - 1) & (~((round) - 1)))

/* 类型判断 */
#define NFS_IS_DIR(inode)       ((inode)->ftype == NFS_DIR)
#define NFS_IS_REG(inode)       ((inode)->ftype == NFS_REG_FILE)

/******************************************************************************
* SECTION: newfs.c
*******************************************************************************/
void* 			   newfs_init(struct fuse_conn_info *);
void  			   newfs_destroy(void *);
int   			   newfs_mkdir(const char *, mode_t);
int   			   newfs_getattr(const char *, struct stat *);
int   			   newfs_readdir(const char *, void *, fuse_fill_dir_t, off_t,
						                struct fuse_file_info *);
int   			   newfs_mknod(const char *, mode_t, dev_t);
int   			   newfs_write(const char *, const char *, size_t, off_t,
					                  struct fuse_file_info *);
int   			   newfs_read(const char *, char *, size_t, off_t,
					                 struct fuse_file_info *);
int   			   newfs_access(const char *, int);
int   			   newfs_unlink(const char *);
int   			   newfs_rmdir(const char *);
int   			   newfs_rename(const char *, const char *);
int   			   newfs_utimens(const char *, const struct timespec tv[2]);
int   			   newfs_truncate(const char *, off_t);
			
int   			   newfs_open(const char *, struct fuse_file_info *);
int   			   newfs_opendir(const char *, struct fuse_file_info *);

/* 辅助函数 */
int                newfs_driver_read(int offset, uint8_t *out_content, int size);
int                newfs_driver_write(int offset, uint8_t *in_content, int size);
int                newfs_alloc_data_block();
void               newfs_free_data_block(int block_no);

#endif  /* _newfs_H_ */