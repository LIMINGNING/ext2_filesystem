#ifndef _TYPES_H_
#define _TYPES_H_

#define MAX_NAME_LEN    128
#define NFS_DATA_PER_FILE 6
#include <stdbool.h>

#define SFS_ASSIGN_FNAME(psfs_dentry, _fname) \
    memcpy(psfs_dentry->name, _fname, strlen(_fname))

typedef enum newfs_file_type
{
    NFS_REG_FILE,
    NFS_DIR,
    NFS_SYM_LINK
} NFS_FILE_TYPE;

struct newfs_super;
struct newfs_inode;
struct newfs_dentry;

struct custom_options {
	const char*        device;
};

struct newfs_super
{
    int fd;

    int sz_io;  /* = 512B */
    int sz_disk; /* = 4MB */
    int sz_usage;
    int sz_blks; /* = 1024B */

    int blks_num; /* 4MB / 1024B = 4096 */

    /* super block */
    int sb_offset;  /* offset = 0 */
    int sb_blks;  /* = 1 */

    uint8_t *map_inode;
    int ino_bitmap_offset;  /* offset = 1 */
    int ino_bitmap_blks;  /* = 1 */

    uint8_t *map_data;
    int data_bitmap_offset; /* offset = 2 */
    int data_bitmap_blks;  /* = 1 */

    /* Struct inode */
    int inode_blks;    /* Size of inode map (block) */
    int inode_offset;  /* offset of inode */
    
    int data_offset;
    int data_blks;
    
    int max_ino;
    int file_max;

    bool is_mounted;

    int root_ino;
    struct newfs_dentry *root_dentry;
};

struct newfs_inode
{
    uint32_t ino;
    uint32_t size;       /* 统一使用 uint32_t */
    uint32_t dir_cnt;    /* 统一使用 uint32_t */
    NFS_FILE_TYPE ftype; /* 添加文件类型字段 */
    char target_path[MAX_NAME_LEN];
    struct newfs_dentry *dentry;  /* 指向该inode的dentry */
    struct newfs_dentry *dentrys; /* 所有目录项 */
    uint32_t block_pointer[NFS_DATA_PER_FILE]; /* 磁盘块号数组（动态分配） */
    uint8_t *data;
};

struct newfs_dentry {
    char     name[MAX_NAME_LEN];
    uint32_t ino;
    /* TODO: Define yourself */
    struct newfs_dentry *parent;  /* 父亲Inode的dentry */
    struct newfs_dentry *brother; /* 兄弟 */
    struct newfs_inode *inode;    /* 指向inode */
    NFS_FILE_TYPE ftype;
};

/* static inline struct newfs_dentry *new_dentry(char *fname, NFS_FILE_TYPE ftype)
{
    struct newfs_dentry *dentry = (struct newfs_dentry *)malloc(sizeof(struct newfs_dentry));
    memset(dentry, 0, sizeof(struct newfs_dentry));
    // SFS_ASSIGN_FNAME(dentry, fname);
    memcpy(dentry, fname, sizeof(fname));
    dentry->ftype = ftype;
    dentry->ino = -1;
    dentry->inode = NULL;
    dentry->parent = NULL;
    dentry->brother = NULL;
} */

struct newfs_super_d
{
    uint32_t magic_number;
    int sz_usage;

    int sz_blks; /* = 1024B */
    int blks_num; /* 4MB / 1024B = 4096 */

    /* super block */
    int sb_offset; /* offset = 0 */
    int sb_blks;   /* = 1 */

    /* inode bitmap */
    int ino_bitmap_offset; /* offset = 1 */
    int ino_bitmap_blks;          /* = 1 */

    /* data bitmap */
    int data_bitmap_offset; /* offset = 2 */
    int data_bitmap_blks;          /* = 1 */

    /* inode region */
    int inode_offset;
    int inode_blks;

    /* data region */
    int data_offset;
    int data_blks;

    int max_ino;        /* 最大支持的inode数 */
    int file_max;       /* 支持文件的最大大小 */

    int root_ino;
};

struct newfs_inode_d
{
    uint32_t ino;                        /* 在inode位图中的下标 */
    uint32_t size;                       /* 文件已占用空间 */
    char target_path[MAX_NAME_LEN];      /* store traget path when it is a symlink */

    uint32_t block_pointer[NFS_DATA_PER_FILE];
    uint32_t dir_cnt;
    NFS_FILE_TYPE ftype;
};

struct newfs_dentry_d
{
    char fname[MAX_NAME_LEN];
    NFS_FILE_TYPE ftype;
    uint32_t ino; /* 指向的ino号 */
};

#endif /* _TYPES_H_ */