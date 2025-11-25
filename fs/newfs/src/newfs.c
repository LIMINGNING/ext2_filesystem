#define _XOPEN_SOURCE 700

#include "newfs.h"

/******************************************************************************
* SECTION: 宏定义
*******************************************************************************/
#define OPTION(t, p)        { t, offsetof(struct custom_options, p), 1 }

/******************************************************************************
* SECTION: 全局变量
*******************************************************************************/
static const struct fuse_opt option_spec[] = {		/* 用于FUSE文件系统解析参数 */
	OPTION("--device=%s", device),
	FUSE_OPT_END
};

struct custom_options newfs_options;			 /* 全局选项 */
struct newfs_super super;

/* 函数声明 */
struct newfs_dentry *newfs_alloc_dentry(const char *name, NFS_FILE_TYPE ftype);
struct newfs_inode *newfs_alloc_inode(struct newfs_dentry *dentry);
int newfs_alloc_ino();
int newfs_sync_inode(struct newfs_inode *inode);
struct newfs_inode *newfs_read_inode(struct newfs_dentry *dentry, int ino);
int newfs_alloc_dentry_to_inode(struct newfs_inode *inode, struct newfs_dentry *dentry);
struct newfs_dentry *newfs_get_dentry(struct newfs_inode *inode, int dir_index);
char *newfs_get_fname(const char *path);
struct newfs_dentry *newfs_lookup(const char *path, bool *is_find, bool *is_root);

/* 辅助函数 */
int newfs_read_block(int fd, int block_no, uint8_t *buf);
int newfs_write_block(int fd, int block_no, uint8_t *buf);

/******************************************************************************
* SECTION: FUSE操作定义
*******************************************************************************/
static struct fuse_operations operations = {
	.init = newfs_init,						 /* mount文件系统 */		
	.destroy = newfs_destroy,				 /* umount文件系统 */
	.mkdir = newfs_mkdir,					 /* 建目录，mkdir */
	.getattr = newfs_getattr,				 /* 获取文件属性，类似stat，必须完成 */
	.readdir = newfs_readdir,				 /* 填充dentrys */
	.mknod = newfs_mknod,					 /* 创建文件，touch相关 */
	.write = NULL,								  	 /* 写入文件 */
	.read = NULL,								  	 /* 读文件 */
	.utimens = newfs_utimens,				 /* 修改时间，忽略，避免touch报错 */
	.truncate = NULL,						  		 /* 改变文件大小 */
	.unlink = NULL,							  		 /* 删除文件 */
	.rmdir	= NULL,							  		 /* 删除目录， rm -r */
	.rename = NULL,							  		 /* 重命名，mv */

	.open = NULL,							
	.opendir = NULL,
	.access = NULL
};
/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 挂载（mount）文件系统
 * 
 * @param conn_info 可忽略，一些建立连接相关的信息 
 * @return void*
 */
void *newfs_init(struct fuse_conn_info *conn_info)
{
    int ret;
    struct newfs_super_d super_d;
    uint8_t *temp_buf;
    bool is_init = false;

    /* 打开驱动 */
    super.fd = ddriver_open(newfs_options.device);
    if (super.fd < 0) {
        return NULL;
    }

    /* 获取磁盘信息 */
    ddriver_ioctl(super.fd, IOC_REQ_DEVICE_SIZE, &super.sz_disk);
    ddriver_ioctl(super.fd, IOC_REQ_DEVICE_IO_SZ, &super.sz_io);

    super.sz_blks = 1024;
    super.blks_num = super.sz_disk / super.sz_blks;

    /* 读取超级块 */
    temp_buf = (uint8_t *)malloc(NFS_BLKS_SZ());
    ret = newfs_read_block(super.fd, 0, temp_buf);
    if (ret < 0) {
        free(temp_buf);
        return NULL;
    }
    memcpy(&super_d, temp_buf, sizeof(struct newfs_super_d));
    free(temp_buf);

    /* 判断是否首次挂载 */
    if (super_d.magic_number != NFS_MAGIC_NUM) {
        /******************************************************************************
         * SECTION: 首次挂载 - 计算布局（直接在 super_d 上操作）
         ******************************************************************************/
        
        /* 布局计算：
         * sizeof(struct newfs_inode_d) = 4 + 4 + 128 + (4*6) + 4 + 4 = 168 字节
         *   - ino(4) + size(4) + target_path(128) + block_pointer[6](24) + dir_cnt(4) + ftype(4)
         * 平均每个文件 = 4 个数据块 + 1 个 inode = 4*1024 + 168 = 4264 字节
         * 最大 inode 数 = 4096*1024 / 4264 = 983.48 ≈ 983
         * inode 区域块数 = ceil(983 * 168 / 1024) = ceil(161.48) = 162 块
         */
        int avg_file_size = 4 * NFS_BLKS_SZ() + sizeof(struct newfs_inode_d);  // 4264
        int max_ino = (super.blks_num * NFS_BLKS_SZ()) / avg_file_size;        // 983

        /* 直接在 super_d 上计算布局 */
        super_d.magic_number = NFS_MAGIC_NUM;
        super_d.sz_usage = 0;
        super_d.sz_blks = super.sz_blks;
        super_d.blks_num = super.blks_num;
        
        super_d.sb_offset = 0;
        super_d.sb_blks = 1;
        
        super_d.ino_bitmap_offset = 1;
        super_d.ino_bitmap_blks = 1;
        
        super_d.data_bitmap_offset = 2;
        super_d.data_bitmap_blks = 1;
        
        super_d.inode_offset = 3;
        super_d.inode_blks = (max_ino * sizeof(struct newfs_inode_d)) / NFS_BLKS_SZ() + 1;
        
        super_d.data_offset = super_d.inode_offset + super_d.inode_blks;
        super_d.data_blks = super.blks_num - super_d.data_offset;
        
        super_d.max_ino = max_ino;
        super_d.file_max = NFS_DATA_PER_FILE * NFS_BLKS_SZ();
        super_d.root_ino = 0;
        
        is_init = true;
    }

    /******************************************************************************
     * SECTION: 统一从 super_d 恢复到 super（无论首次还是非首次）
     ******************************************************************************/
    
    super.sz_usage = super_d.sz_usage;
    super.sb_offset = super_d.sb_offset;
    super.sb_blks = super_d.sb_blks;
    
    super.ino_bitmap_offset = super_d.ino_bitmap_offset;
    super.ino_bitmap_blks = super_d.ino_bitmap_blks;
    
    super.data_bitmap_offset = super_d.data_bitmap_offset;
    super.data_bitmap_blks = super_d.data_bitmap_blks;
    
    super.inode_offset = super_d.inode_offset;
    super.inode_blks = super_d.inode_blks;
    
    super.data_offset = super_d.data_offset;
    super.data_blks = super_d.data_blks;
    
    super.max_ino = super_d.max_ino;
    super.file_max = super_d.file_max;
    super.root_ino = super_d.root_ino;

    /* 分配位图内存 */
    super.map_inode = (uint8_t *)malloc(NFS_BLKS_SZ());
    super.map_data = (uint8_t *)malloc(NFS_BLKS_SZ());

    if (is_init) {
        /******************************************************************************
         * SECTION: 首次挂载 - 初始化位图和根目录
         ******************************************************************************/
        
        /* 清空位图 */
        memset(super.map_inode, 0, NFS_BLKS_SZ());
        memset(super.map_data, 0, NFS_BLKS_SZ());
        
        /* 创建根目录 */
        super.root_dentry = newfs_alloc_dentry("/", NFS_DIR);
        super.root_dentry->inode = newfs_alloc_inode(super.root_dentry);
        
        /* 标记根 inode 已使用 */
        super.map_inode[0] |= 0x01;
        
        /* 将超级块写回磁盘 */
        temp_buf = (uint8_t *)calloc(1, NFS_BLKS_SZ());
        memcpy(temp_buf, &super_d, sizeof(struct newfs_super_d));
        ret = newfs_write_block(super.fd, 0, temp_buf);
        free(temp_buf);
        
        /* 写入位图 */
        ret = newfs_write_block(super.fd, super.ino_bitmap_offset, super.map_inode);
        ret = newfs_write_block(super.fd, super.data_bitmap_offset, super.map_data);
        
        /* 写入根 inode */
        newfs_sync_inode(super.root_dentry->inode);
    }
    else {
        /******************************************************************************
         * SECTION: 非首次挂载 - 读取位图和根目录
         ******************************************************************************/
        
        /* 读取位图 */
        ret = newfs_read_block(super.fd, super.ino_bitmap_offset, super.map_inode);
        ret = newfs_read_block(super.fd, super.data_bitmap_offset, super.map_data);
        
        /* 读取根目录 */
        super.root_dentry = newfs_alloc_dentry("/", NFS_DIR);
        super.root_dentry->inode = newfs_read_inode(super.root_dentry, super.root_ino);
    }

    super.is_mounted = true;
    return NULL;
}
/**
 * @brief 卸载（umount）文件系统
 *
 * @param p 可忽略
 * @return void
 */
void newfs_destroy(void *p)
{
    if (!super.is_mounted)
    {
        return;
    }

    struct newfs_super_d super_d;

    /******************************************************************************
     * SECTION: 1. 从根节点向下递归刷写所有 inode（包括目录项和文件数据）
     ******************************************************************************/
    newfs_sync_inode(super.root_dentry->inode);

    /******************************************************************************
     * SECTION: 2. 将内存超级块写回磁盘
     ******************************************************************************/
    super_d.magic_number = NFS_MAGIC_NUM;
    super_d.sz_usage = super.sz_usage;
    super_d.sz_blks = super.sz_blks;
    super_d.blks_num = super.blks_num;

    super_d.sb_offset = super.sb_offset;
    super_d.sb_blks = super.sb_blks;

    super_d.ino_bitmap_offset = super.ino_bitmap_offset;
    super_d.ino_bitmap_blks = super.ino_bitmap_blks;

    super_d.data_bitmap_offset = super.data_bitmap_offset;
    super_d.data_bitmap_blks = super.data_bitmap_blks;

    super_d.inode_offset = super.inode_offset;
    super_d.inode_blks = super.inode_blks;

    super_d.data_offset = super.data_offset;
    super_d.data_blks = super.data_blks;

    super_d.max_ino = super.max_ino;
    super_d.file_max = super.file_max;
    super_d.root_ino = super.root_ino;

    /* 写入超级块到磁盘 */
    if (newfs_driver_write(NFS_SUPER_OFS, (uint8_t *)&super_d,
                           sizeof(struct newfs_super_d)) != NFS_ERROR_NONE)
    {
        printf("[NEWFS] Error: Failed to write super block\n");
    }

    /******************************************************************************
     * SECTION: 3. 写回 inode 位图
     ******************************************************************************/
    if (newfs_driver_write(super.ino_bitmap_offset * NFS_BLKS_SZ(),
                           (uint8_t *)(super.map_inode),
                           NFS_BLKS_SZ()) != NFS_ERROR_NONE)
    {
        printf("[NEWFS] Error: Failed to write inode bitmap\n");
    }

    /******************************************************************************
     * SECTION: 4. 写回数据块位图
     ******************************************************************************/
    if (newfs_driver_write(super.data_bitmap_offset * NFS_BLKS_SZ(),
                           (uint8_t *)(super.map_data),
                           NFS_BLKS_SZ()) != NFS_ERROR_NONE)
    {
        printf("[NEWFS] Error: Failed to write data bitmap\n");
    }

    /******************************************************************************
     * SECTION: 5. 释放内存
     ******************************************************************************/
    free(super.map_inode);
    free(super.map_data);

    /******************************************************************************
     * SECTION: 6. 关闭驱动
     ******************************************************************************/
    ddriver_close(super.fd);

    super.is_mounted = false;

    return;
}

/**
 * @brief 创建目录
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建模式（只读？只写？），可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_mkdir(const char* path, mode_t mode) {
	(void)mode;
	bool is_find, is_root;
	char* fname;
	struct newfs_dentry* last_dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* dentry;

	if (is_find) {
		return -NFS_ERROR_EXISTS;
	}

	if (last_dentry == NULL) {
		return -NFS_ERROR_NOTFOUND;
	}

	if (last_dentry->inode == NULL) {
		return -NFS_ERROR_NOTFOUND;
	}

	if (NFS_IS_REG(last_dentry->inode)) {
		return -NFS_ERROR_UNSUPPORTED;
	}

	fname  = newfs_get_fname(path);
	dentry = newfs_alloc_dentry(fname, NFS_DIR); 
	dentry->parent = last_dentry;
	newfs_alloc_inode(dentry);
	newfs_alloc_dentry_to_inode(last_dentry->inode, dentry);
	
	return NFS_ERROR_NONE;
}

/**
 * @brief 获取文件或目录的属性，该函数非常重要
 * 
 * @param path 相对于挂载点的路径
 * @param newfs_stat 返回状态
 * @return int 0成功，否则返回对应错误号
 */
int newfs_getattr(const char* path, struct stat * newfs_stat) {
	bool is_find, is_root;
	struct newfs_dentry* dentry = newfs_lookup(path, &is_find, &is_root);
	
	if (is_find == false) {
		return -NFS_ERROR_NOTFOUND;
	}

	/* 根据文件类型填充 stat 结构 */
	if (NFS_IS_DIR(dentry->inode)) {
		newfs_stat->st_mode = S_IFDIR | 0777;
		newfs_stat->st_size = dentry->inode->dir_cnt * sizeof(struct newfs_dentry_d);
	}
	else if (NFS_IS_REG(dentry->inode)) {
		newfs_stat->st_mode = S_IFREG | 0777;
		newfs_stat->st_size = dentry->inode->size;
	}

	newfs_stat->st_nlink = 1;
	newfs_stat->st_uid = getuid();
	newfs_stat->st_gid = getgid();
	newfs_stat->st_atime = time(NULL);
	newfs_stat->st_mtime = time(NULL);
	newfs_stat->st_blksize = NFS_BLKS_SZ();

	/* 根目录特殊处理 */
	if (is_root) {
		newfs_stat->st_size = super.sz_usage;
		newfs_stat->st_blocks = super.sz_disk / NFS_IO_SZ();
		newfs_stat->st_nlink = 2;  /* 根目录 link 数为 2 */
	}

	return NFS_ERROR_NONE;
}

/**
 * @brief 遍历目录项，填充至buf，并交给FUSE输出
 * 
 * @param path 相对于挂载点的路径
 * @param buf 输出buffer
 * @param filler 参数讲解:
 * 
 * typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
 *				const struct stat *stbuf, off_t off)
 * buf: name会被复制到buf中
 * name: dentry名字
 * stbuf: 文件状态，可忽略
 * off: 下一次offset从哪里开始，这里可以理解为第几个dentry
 * 
 * @param offset 第几个目录项？
 * @param fi 可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset,
			    		 struct fuse_file_info * fi) {
	bool is_find, is_root;
	int cur_dir = offset;

	struct newfs_dentry* dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* sub_dentry;
	struct newfs_inode* inode;
	
	if (is_find) {
		inode = dentry->inode;
		sub_dentry = newfs_get_dentry(inode, cur_dir);
		if (sub_dentry) {
			filler(buf, sub_dentry->name, NULL, ++offset);
		}
		return NFS_ERROR_NONE;
	}
	return -NFS_ERROR_NOTFOUND;
}

/**
 * @brief 创建文件
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建文件的模式，可忽略
 * @param dev 设备类型，可忽略
 * @return int 0成功，否则返回对应错误号
 */
int newfs_mknod(const char* path, mode_t mode, dev_t dev) {
	bool is_find, is_root;
	
	struct newfs_dentry* last_dentry = newfs_lookup(path, &is_find, &is_root);
	struct newfs_dentry* dentry;
	char* fname;
	
	if (is_find == true) {
		return -NFS_ERROR_EXISTS;
	}

	if (last_dentry == NULL) {
		return -NFS_ERROR_NOTFOUND;
	}

	if (last_dentry->inode == NULL) {
		return -NFS_ERROR_NOTFOUND;
	}

	fname = newfs_get_fname(path);
	
	if (S_ISREG(mode)) {
		dentry = newfs_alloc_dentry(fname, NFS_REG_FILE);
	}
	else if (S_ISDIR(mode)) {
		dentry = newfs_alloc_dentry(fname, NFS_DIR);
	}
	else {
		dentry = newfs_alloc_dentry(fname, NFS_REG_FILE);
	}
	dentry->parent = last_dentry;
	newfs_alloc_inode(dentry);
	newfs_alloc_dentry_to_inode(last_dentry->inode, dentry);

	return NFS_ERROR_NONE;
}

/**
 * @brief 修改时间，为了不让touch报错 
 * 
 * @param path 相对于挂载点的路径
 * @param tv 实践
 * @return int 0成功，否则返回对应错误号
 */
int newfs_utimens(const char* path, const struct timespec tv[2]) {
	(void)path;
	return 0;
}
/******************************************************************************
* SECTION: 选做函数实现
*******************************************************************************/
/**
 * @brief 写入文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 写入大小
 */
int newfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
	/* 选做 */
	return size;
}

/**
 * @brief 读取文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 读取大小
 */
int newfs_read(const char* path, char* buf, size_t size, off_t offset,
		       struct fuse_file_info* fi) {
	/* 选做 */
	return size;			   
}

/**
 * @brief 删除文件
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_unlink(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 删除目录
 * 
 * 一个可能的删除目录操作如下：
 * rm ./tests/mnt/j/ -r
 *  1) Step 1. rm ./tests/mnt/j/j
 *  2) Step 2. rm ./tests/mnt/j
 * 即，先删除最深层的文件，再删除目录文件本身
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rmdir(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 重命名文件 
 * 
 * @param from 源文件路径
 * @param to 目标文件路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rename(const char* from, const char* to) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开文件，可以在这里维护fi的信息，例如，fi->fh可以理解为一个64位指针，可以把自己想保存的数据结构
 * 保存在fh中
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_open(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开目录文件
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_opendir(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 改变文件大小
 * 
 * @param path 相对于挂载点的路径
 * @param offset 改变后文件大小
 * @return int 0成功，否则返回对应错误号
 */
int newfs_truncate(const char* path, off_t offset) {
	/* 选做 */
	return 0;
}


/**
 * @brief 访问文件，因为读写文件时需要查看权限
 * 
 * @param path 相对于挂载点的路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 0成功，否则返回对应错误号
 */
int newfs_access(const char* path, int type) {
	/* 选做: 解析路径，判断是否存在 */
	return 0;
}	
/******************************************************************************
* SECTION: FUSE入口
*******************************************************************************/
int main(int argc, char **argv)
{
    int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    newfs_options.device = strdup("/home/li/user-land-filesystem/driver/user_ddriver/bin/ddriver");

    if (fuse_opt_parse(&args, &newfs_options, option_spec, NULL) == -1)
		return -1;
	
	ret = fuse_main(args.argc, args.argv, &operations, NULL);
	fuse_opt_free_args(&args);
	return ret;
}

/* 辅助函数：读取一个逻辑块（分两次读取 512B） */
int newfs_read_block(int fd, int block_no, uint8_t *buf) {
    int ret;
    int offset = block_no * NFS_BLKS_SZ();
    
    /* 第一次读取前 512B */
    ret = ddriver_seek(fd, offset, SEEK_SET);
    if (ret < 0) return ret;
    ret = ddriver_read(fd, buf, NFS_IO_SZ());
    if (ret < 0) return ret;
    
    /* 第二次读取后 512B */
    ret = ddriver_seek(fd, offset + NFS_IO_SZ(), SEEK_SET);
    if (ret < 0) return ret;
    ret = ddriver_read(fd, buf + NFS_IO_SZ(), NFS_IO_SZ());
    
    return ret;
}

/* 辅助函数：写入一个逻辑块（分两次写入 512B） */
int newfs_write_block(int fd, int block_no, uint8_t *buf) {
    int ret;
    int offset = block_no * NFS_BLKS_SZ();
    
    /* 第一次写入前 512B */
    ret = ddriver_seek(fd, offset, SEEK_SET);
    if (ret < 0) return ret;
    ret = ddriver_write(fd, buf, NFS_IO_SZ());
    if (ret < 0) return ret;
    
    /* 第二次写入后 512B */
    ret = ddriver_seek(fd, offset + NFS_IO_SZ(), SEEK_SET);
    if (ret < 0) return ret;
    ret = ddriver_write(fd, buf + NFS_IO_SZ(), NFS_IO_SZ());
    
    return ret;
}

/**
 * @brief 驱动读（处理对齐）
 */
int newfs_driver_read(int offset, uint8_t *out_content, int size) {
    int offset_aligned = NFS_ROUND_DOWN(offset, NFS_IO_SZ());
    int bias = offset - offset_aligned;
    int size_aligned = NFS_ROUND_UP((size + bias), NFS_IO_SZ());
    uint8_t *temp_content = (uint8_t *)malloc(size_aligned);
    uint8_t *cur = temp_content;
    
    ddriver_seek(super.fd, offset_aligned, SEEK_SET);
    while (size_aligned != 0) {
        ddriver_read(super.fd, cur, NFS_IO_SZ());
        cur += NFS_IO_SZ();
        size_aligned -= NFS_IO_SZ();
    }
    
    memcpy(out_content, temp_content + bias, size);
    free(temp_content);
    return NFS_ERROR_NONE;
}

/**
 * @brief 驱动写（处理对齐）
 */
int newfs_driver_write(int offset, uint8_t *in_content, int size) {
    int offset_aligned = NFS_ROUND_DOWN(offset, NFS_IO_SZ());
    int bias = offset - offset_aligned;
    int size_aligned = NFS_ROUND_UP((size + bias), NFS_IO_SZ());
    uint8_t *temp_content = (uint8_t *)malloc(size_aligned);
    uint8_t *cur = temp_content;
    
    newfs_driver_read(offset_aligned, temp_content, size_aligned);
    memcpy(temp_content + bias, in_content, size);
    
    ddriver_seek(super.fd, offset_aligned, SEEK_SET);
    while (size_aligned != 0) {
        ddriver_write(super.fd, cur, NFS_IO_SZ());
        cur += NFS_IO_SZ();
        size_aligned -= NFS_IO_SZ();
    }
    
    free(temp_content);
    return NFS_ERROR_NONE;
}

/**
 * @brief 分配一个 dentry
 */
struct newfs_dentry *newfs_alloc_dentry(const char *name, NFS_FILE_TYPE ftype)
{
	struct newfs_dentry *dentry = (struct newfs_dentry *)malloc(sizeof(struct newfs_dentry));
	if (dentry == NULL)
	{
		return NULL;
	}

	memset(dentry, 0, sizeof(struct newfs_dentry));
	strncpy(dentry->name, name, MAX_NAME_LEN - 1);
	dentry->name[MAX_NAME_LEN - 1] = '\0';
	dentry->ftype = ftype;  // 设置文件类型
	dentry->ino = -1; // 未分配
	dentry->inode = NULL;
	dentry->parent = NULL;
	dentry->brother = NULL;

	return dentry;
}

/**
 * @brief 分配一个 inode
 */
struct newfs_inode *newfs_alloc_inode(struct newfs_dentry *dentry)
{
	struct newfs_inode *inode = (struct newfs_inode *)malloc(sizeof(struct newfs_inode));
	if (inode == NULL)
	{
		return NULL;
	}

	memset(inode, 0, sizeof(struct newfs_inode));

	/* 分配 inode 编号 */
	int ino = newfs_alloc_ino();
	if (ino == -1)
	{
		free(inode);
		return NULL;
	}

	inode->ino = ino;
	inode->size = 0;
	inode->dir_cnt = 0;
	inode->ftype = dentry->ftype;  /* 使用 dentry 的文件类型 */
	memset(inode->target_path, 0, MAX_NAME_LEN);
	inode->dentry = dentry;
	inode->dentrys = NULL;
	inode->data = NULL;  /* 初始化数据缓存指针 */

	/* 初始化数据块指针为 0（未分配） */
	for (int i = 0; i < NFS_DATA_PER_FILE; i++)
	{
		inode->block_pointer[i] = 0;
	}

	dentry->ino = ino;
	dentry->inode = inode;

	return inode;
}

/**
 * @brief 分配一个 inode 编号
 */
int newfs_alloc_ino()
{
	/* 在位图中查找空闲的 inode */
	for (int byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(); byte_cursor++)
	{
		for (int bit_cursor = 0; bit_cursor < 8; bit_cursor++)
		{
			int ino = byte_cursor * 8 + bit_cursor;
			if (ino >= super.max_ino)
			{
				return -1; // 没有空闲 inode
			}

			/* 检查该位是否为 0（空闲） */
			if ((super.map_inode[byte_cursor] & (1 << bit_cursor)) == 0)
			{
				/* 标记为已使用 */
				super.map_inode[byte_cursor] |= (1 << bit_cursor);
				return ino;
			}
		}
	}
	return -1;
}

/**
 * @brief 分配一个数据块
 */
int newfs_alloc_data_block() {
    /* 在位图中查找空闲的数据块 */
    for (int byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(); byte_cursor++) {
        for (int bit_cursor = 0; bit_cursor < 8; bit_cursor++) {
            int blk_idx = byte_cursor * 8 + bit_cursor;
            if (blk_idx >= super.data_blks) {
                return -1;  // 没有空闲数据块
            }

            /* 检查该位是否为 0（空闲） */
            if ((super.map_data[byte_cursor] & (1 << bit_cursor)) == 0) {
                /* 标记为已使用 */
                super.map_data[byte_cursor] |= (1 << bit_cursor);
                return super.data_offset + blk_idx;  // 返回实际块号
            }
        }
    }
    return -1;
}

/**
 * @brief 释放一个数据块
 */
void newfs_free_data_block(int block_no) {
    if (block_no < super.data_offset || block_no >= super.data_offset + super.data_blks) {
        return;  // 无效的块号
    }
    
    int blk_idx = block_no - super.data_offset;
    int byte_idx = blk_idx / 8;
    int bit_idx = blk_idx % 8;
    
    /* 清除位图中的对应位 */
    super.map_data[byte_idx] &= ~(1 << bit_idx);
}

/**
 * @brief 将内存 inode 及其下方结构全部刷回磁盘
 */
int newfs_sync_inode(struct newfs_inode *inode)
{
    struct newfs_inode_d inode_d;
    struct newfs_dentry *dentry_cursor;
    struct newfs_dentry_d dentry_d;
    int ino = inode->ino;
    int offset;

    /* 先处理目录的数据块分配（在写入 inode 之前） */
    if (NFS_IS_DIR(inode))
    {
        /* 如果是目录，数据是目录项 */
        /* 为目录分配第一个数据块（如果还没分配） */
        if (inode->dir_cnt > 0 && inode->block_pointer[0] == 0)
        {
            int block_no = newfs_alloc_data_block();
            if (block_no == -1)
            {
                return -NFS_ERROR_NOSPACE;
            }
            inode->block_pointer[0] = block_no;
        }
    }

    /* 填充磁盘 inode 结构 */
    inode_d.ino = ino;
    inode_d.size = inode->size;
    inode_d.dir_cnt = inode->dir_cnt;
    inode_d.ftype = inode->ftype;
    memcpy(inode_d.target_path, inode->target_path, MAX_NAME_LEN);

    /* 复制数据块指针（此时 block_pointer 已经分配好了） */
    for (int i = 0; i < NFS_DATA_PER_FILE; i++)
    {
        inode_d.block_pointer[i] = inode->block_pointer[i];
    }

    /* 写 inode 到磁盘 */
    if (newfs_driver_write(NFS_INO_OFS(ino), (uint8_t *)&inode_d,
                           sizeof(struct newfs_inode_d)) != NFS_ERROR_NONE)
    {
        return -NFS_ERROR_IO;
    }

    /* 写 inode 下方的数据 */
    if (NFS_IS_DIR(inode))
    {
        /* 写入目录项到数据块 */
        if (inode->block_pointer[0] != 0)
        {
            dentry_cursor = inode->dentrys;
            offset = inode->block_pointer[0] * NFS_BLKS_SZ();

            while (dentry_cursor != NULL)
            {
                memcpy(dentry_d.fname, dentry_cursor->name, MAX_NAME_LEN);
                dentry_d.ftype = dentry_cursor->ftype;
                dentry_d.ino = dentry_cursor->ino;

                if (newfs_driver_write(offset, (uint8_t *)&dentry_d,
                                       sizeof(struct newfs_dentry_d)) != NFS_ERROR_NONE)
                {
                    return -NFS_ERROR_IO;
                }

                /* 递归写入子 inode */
                if (dentry_cursor->inode != NULL)
                {
                    newfs_sync_inode(dentry_cursor->inode);
                }

                dentry_cursor = dentry_cursor->brother;
                offset += sizeof(struct newfs_dentry_d);
            }
        }
    }
    else if (NFS_IS_REG(inode))
    {
        /* 如果是文件，数据块已经在 write 操作时分配和写入 */
        /* 这里只需要确保 block_pointer 已经记录在 inode_d 中 */
    }

    return NFS_ERROR_NONE;
}

/**
 * @brief 从磁盘读取 inode
 */
struct newfs_inode *newfs_read_inode(struct newfs_dentry *dentry, int ino)
{
    struct newfs_inode *inode = (struct newfs_inode *)malloc(sizeof(struct newfs_inode));
    struct newfs_inode_d inode_d;
    struct newfs_dentry *sub_dentry;
    struct newfs_dentry_d dentry_d;
    int dir_cnt = 0, i;

    if (inode == NULL)
    {
        return NULL;
    }

    /* 从磁盘读索引节点 */
    if (newfs_driver_read(NFS_INO_OFS(ino), (uint8_t *)&inode_d,
                          sizeof(struct newfs_inode_d)) != NFS_ERROR_NONE)
    {
        free(inode);
        return NULL;
    }

    /* 填充内存 inode 结构 */
    inode->ino = inode_d.ino;
    inode->size = inode_d.size;
    inode->dir_cnt = 0;
    inode->ftype = inode_d.ftype;
    memcpy(inode->target_path, inode_d.target_path, MAX_NAME_LEN);
    inode->dentry = dentry;
    inode->dentrys = NULL;
    inode->data = NULL;  /* 初始化数据缓存指针 */

    /* 复制数据块指针 */
    for (int i = 0; i < NFS_DATA_PER_FILE; i++)
    {
        inode->block_pointer[i] = inode_d.block_pointer[i];
    }

    /* 读取 inode 的数据或子目录项 */
    if (NFS_IS_DIR(inode))
    {
        dir_cnt = inode_d.dir_cnt;
        /* 从第一个数据块读取目录项 */
        if (dir_cnt > 0 && inode->block_pointer[0] != 0)
        {
            for (i = 0; i < dir_cnt; i++)
            {
                if (newfs_driver_read(inode->block_pointer[0] * NFS_BLKS_SZ() + i * sizeof(struct newfs_dentry_d),
                                      (uint8_t *)&dentry_d,
                                      sizeof(struct newfs_dentry_d)) != NFS_ERROR_NONE)
                {
                    return NULL;
                }

                sub_dentry = newfs_alloc_dentry(dentry_d.fname, dentry_d.ftype);
                sub_dentry->parent = inode->dentry;
                sub_dentry->ino = dentry_d.ino;
                newfs_alloc_dentry_to_inode(inode, sub_dentry);
            }
        }
    }
    else if (NFS_IS_REG(inode))
    {
        /* 文件的数据块会在 read 操作时按需读取 */
        /* 这里不预先读取，节省内存 */
    }

    return inode;
}

/**
 * @brief 将 dentry 插入到 inode 中（头插法）
 */
int newfs_alloc_dentry_to_inode(struct newfs_inode *inode, struct newfs_dentry *dentry)
{
    if (inode->dentrys == NULL)
    {
        inode->dentrys = dentry;
    }
    else
    {
        dentry->brother = inode->dentrys;
        inode->dentrys = dentry;
    }
    inode->dir_cnt++;
    return inode->dir_cnt;
}

/**
 * @brief 将 dentry 从 inode 的 dentrys 中取出
 */
int newfs_drop_dentry(struct newfs_inode *inode, struct newfs_dentry *dentry)
{
    bool is_find = false;
    struct newfs_dentry *dentry_cursor;

    dentry_cursor = inode->dentrys;

    if (dentry_cursor == dentry)
    {
        inode->dentrys = dentry->brother;
        is_find = true;
    }
    else
    {
        while (dentry_cursor)
        {
            if (dentry_cursor->brother == dentry)
            {
                dentry_cursor->brother = dentry->brother;
                is_find = true;
                break;
            }
            dentry_cursor = dentry_cursor->brother;
        }
    }

    if (!is_find)
    {
        return -NFS_ERROR_NOTFOUND;
    }

    inode->dir_cnt--;
    return inode->dir_cnt;
}

/**
 * @brief 根据偏移获取目录的第 dir_index 个子目录项
 * @param inode 目录的 inode
 * @param dir_index 子目录项的索引（第几个）
 * @return struct newfs_dentry* 返回对应的 dentry，如果不存在返回 NULL
 */
struct newfs_dentry *newfs_get_dentry(struct newfs_inode *inode, int dir_index)
{
    struct newfs_dentry *dentry_cursor = inode->dentrys;
    int cnt = 0;

    if (dir_index < 0 || dir_index >= inode->dir_cnt)
    {
        return NULL;
    }

    while (dentry_cursor)
    {
        if (cnt == dir_index)
        {
            return dentry_cursor;
        }
        cnt++;
        dentry_cursor = dentry_cursor->brother;
    }

    return NULL;
}

/**
 * @brief 获取文件名
 */
char *newfs_get_fname(const char *path)
{
    char ch = '/';
    char *q = strrchr(path, ch) + 1;
    return q;
}

/**
 * @brief 计算路径层级
 */
int newfs_calc_lvl(const char *path)
{
    char *str = (char *)path;
    int lvl = 0;

    if (strcmp(path, "/") == 0)
    {
        return lvl;
    }

    while (*str != '\0')
    {
        if (*str == '/')
        {
            lvl++;
        }
        str++;
    }
    return lvl;
}

/**
 * @brief 路径查找
 * @param path 路径
 * @param is_find 是否找到
 * @param is_root 是否是根目录
 * @return 找到的 dentry 或最后一个有效的 dentry
 */
struct newfs_dentry *newfs_lookup(const char *path, bool *is_find, bool *is_root)
{
    struct newfs_dentry *dentry_cursor = super.root_dentry;
    struct newfs_dentry *dentry_ret = NULL;
    struct newfs_inode *inode;
    int total_lvl = newfs_calc_lvl(path);
    int lvl = 0;
    bool is_hit;
    char *fname = NULL;
    char *path_cpy = (char *)malloc(strlen(path) + 1);

    *is_root = false;
    *is_find = false;
    strcpy(path_cpy, path);

    if (total_lvl == 0)
    {
        *is_find = true;
        *is_root = true;
        dentry_ret = super.root_dentry;
        free(path_cpy);
        return dentry_ret;
    }

    fname = strtok(path_cpy, "/");
    while (fname)
    {
        lvl++;

        if (dentry_cursor->inode == NULL)
        {
            dentry_cursor->inode = newfs_read_inode(dentry_cursor, dentry_cursor->ino);
        }

        inode = dentry_cursor->inode;

        if (NFS_IS_REG(inode) && lvl < total_lvl)
        {
            dentry_ret = inode->dentry;
            break;
        }

        if (NFS_IS_DIR(inode))
        {
            dentry_cursor = inode->dentrys;
            is_hit = false;

            while (dentry_cursor)
            {
                if (strcmp(fname, dentry_cursor->name) == 0)
                {
                    is_hit = true;
                    break;
                }
                dentry_cursor = dentry_cursor->brother;
            }

            if (!is_hit)
            {
                *is_find = false;
                dentry_ret = inode->dentry;
                break;
            }

            if (is_hit && lvl == total_lvl)
            {
                *is_find = true;
                dentry_ret = dentry_cursor;
                break;
            }
        }

        fname = strtok(NULL, "/");
    }

    if (dentry_ret && dentry_ret->inode == NULL)
    {
        dentry_ret->inode = newfs_read_inode(dentry_ret, dentry_ret->ino);
    }

    free(path_cpy);
    return dentry_ret;
}