 
 /*
 * HUST_fs disk layout:
 * 400KB disk -> 100 blocks
 * And can write 100 files at most.
 * inode size is 128B
 * block size is 4096B <=> 4K
 * block0 |dummy block
 * block1 |super block
 * block2 |bmap block
 * block3 |imap block
 * block4-9 |inode table
 * other blocks(block0A-) |data blocks
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <endian.h>
#include <linux/stat.h>

#include "constants.h"

/* 
1. *bmap为8位数组，每一位对应一个block（是否被使用）
2. *imap位8位数组，每一位对应一个inode号（是否被使用）
2. disk_size，以字节为单位（如果dd bs=4096 count=100 if=/dev/zero of=image，那么disk_size的值为409600）
3. bmap_size, imap_size, inode_table_size的单位为blocks的个数（分别为1， 1， 6）
*/
static uint8_t* bmap;
static uint8_t* imap;
static uint64_t disk_size;
static uint64_t bmap_size;
static uint64_t imap_size;
static uint64_t inode_table_size;

struct HUST_fs_super_block {
	uint64_t version;
	uint64_t magic;
	uint64_t block_size;
	uint64_t inodes_count;
	uint64_t free_blocks;
	uint64_t blocks_count;

	uint64_t bmap_block;
	uint64_t imap_block;
	uint64_t inode_table_block;
	uint64_t data_block_number;
	char padding[4016];
};

/* 定义全局变量super_block */
static struct HUST_fs_super_block super_block;

struct HUST_inode {
	mode_t mode;//sizeof(mode_t) is 4
	uint64_t inode_no;
	uint64_t blocks;
	uint64_t block[HUST_N_BLOCKS];
	union {
		uint64_t file_size;
		uint64_t dir_children_count;
	};
    int32_t i_uid; 
    int32_t i_gid;
    int32_t i_nlink;
    
    int64_t i_atime;
    int64_t i_mtime;
    int64_t i_ctime;
    char padding[112];
};

/* 结构体struct HUST_inode的大小为264，即 define HUST_INODE_SIZE 264 */
#define HUST_INODE_SIZE sizeof(struct HUST_inode)


struct HUST_dir_record
{
	char filename[HUST_FILENAME_MAX_LEN];
	uint64_t inode_no;
};

static off_t get_file_size(const char* path)
{
	off_t ret = -1;
	struct stat statbuf;
	if (stat(path, &statbuf) < 0) {
		return ret;
	}
	return statbuf.st_size;
}

/*
1. 参数idx，即第idx个block。此函数完成的是将第idx个block在bmap中对应位置位。
2. array_idx表示idx在bmap数组中的第array_idx项，即bmap[array_idx]。
3. off表示第off位
*/
static int set_bmap(uint64_t idx, int value)
{
	if(!bmap) {
		return -1;
	}
	uint64_t array_idx = idx/(sizeof(char)*8);
	uint64_t off = idx%(sizeof(char)*8);
	if(array_idx > bmap_size*HUST_BLOCKSIZE) {
		printf("Set bmap error and idx is %llu\n", idx);
		return -1;
	}
	if(value)
		bmap[array_idx] |= (1<<off);
	else
		bmap[array_idx] &= ~(1<<off);
	return 0;
}

static void set_imap()
{
	// 根目录和欢迎文件"file"分别占用1个inode，即将imap[0]的前两位置位。
	imap[0] |= 0x3;	
}

static int init_disk(int fd, const char* path)
{
	disk_size = get_file_size(path);
	if (disk_size == -1) {
		perror("Error: can not get disk size!\n");
		return -1;
	}
	printf("Disk size is %lu\n", disk_size);
	super_block.version = 1;
	super_block.magic = MAGIC_NUM;
	super_block.block_size = HUST_BLOCKSIZE;
	super_block.blocks_count = disk_size/HUST_BLOCKSIZE;
	printf("blocks count is %llu\n", super_block.blocks_count);
	super_block.inodes_count = super_block.blocks_count; //将inode个数定义为blocks个数
	super_block.free_blocks = 0;
	
	/* 计算bmap
	共需要1个block存储bmap，即第2个block。
	bmap_size = 100/(8*4096) = 0
	bmap_size += 1
	*/
	bmap_size = super_block.blocks_count/(8*HUST_BLOCKSIZE);
	super_block.bmap_block = RESERVE_BLOCKS;

	if (super_block.blocks_count%(8*HUST_BLOCKSIZE) != 0) {
		bmap_size += 1;
	}
	// 初始化bmap（置0）。
	bmap = (uint8_t *)malloc(bmap_size*HUST_BLOCKSIZE);
	memset(bmap,0,bmap_size*HUST_BLOCKSIZE);

	/* 计算imap
	共需要1个block存储imap，即第3个block。
	imap_size = 100/(8*4096) = 0
	imap_size += 1
	*/
	imap_size = super_block.inodes_count/(8*HUST_BLOCKSIZE);
	super_block.imap_block = super_block.bmap_block + bmap_size;

	if(super_block.inodes_count%(8*HUST_BLOCKSIZE) != 0) {
		imap_size += 1;
	}
	// 初始化imap（置0）
	imap = (uint8_t *)malloc(imap_size*HUST_BLOCKSIZE);
	memset(imap,0,imap_size*HUST_BLOCKSIZE);

	/* 计算inode_table_size
	1. 每个inode大小位264字节，一共100个inode
	2. 共需要6个block储存inode（这6个block即inode table）
	inode_table_size = 100/(4096/264) = 6
	3. 即第4-9个block。
	*/
	inode_table_size = super_block.inodes_count/(HUST_BLOCKSIZE/HUST_INODE_SIZE);
	super_block.inode_table_block = super_block.imap_block + imap_size;
	/* 计算data_block的起始位置
	1. data_block_number = 2 + 1 + 1 + 6 = 10（即0x0A）
	2. 从第0x0A开始至剩下所有的block用于存储data（即第10-100个block）
	*/
	super_block.data_block_number = RESERVE_BLOCKS + bmap_size + imap_size + inode_table_size;
	/* 计算free_block的个数
	1. free_blocks = 100 - 10 - 1 = 89，即0x59
	2. -1是因为根目录占用一个block。
	*/
	super_block.free_blocks = super_block.blocks_count - super_block.data_block_number - 1;

	// 填充bmap，将已使用的block，即dummy block(1), super block(1), bmap block(1), imap block(1), inode table(6)，共计前10个block对应的bmap位置1。
	int idx;
	// plus one becase of the root dir
	for (idx = 0; idx < super_block.data_block_number + 1; ++idx) {
		if (set_bmap(idx, 1)) {
			return -1;
		}
	}

	// 填充imap
	set_imap();
	
	return 0;
}

/* 写dummy block信息到磁盘 */
static int write_dummy(int fd)
{
	char dummy[HUST_BLOCKSIZE] = {0};
	ssize_t res = write(fd, dummy, HUST_BLOCKSIZE);
	if (res != HUST_BLOCKSIZE) {
		perror("write_dummy error!");
		return -1;
	}
	return 0;
}

/* 写super block信息到磁盘 */
static int write_sb(int fd) 
{
	ssize_t ret;
	ret = write(fd, &super_block, sizeof(super_block));
	if(ret != HUST_BLOCKSIZE) {
		perror("Write super block error!\n");
		return -1;
	}
	printf("Super block written succesfully!\n");
	return 0;
}

/* 写bmap信息到磁盘 */
static int write_bmap(int fd) 
{
	ssize_t ret = -1;

	ret = write(fd, bmap, bmap_size*HUST_BLOCKSIZE);
	if (ret != bmap_size*HUST_BLOCKSIZE) {
		perror("Write_bmap() error!\n");
		return -1;
	}
	return 0;

}

/* 写imap信息到磁盘 */
static int write_imap(int fd)
{
	ssize_t res = write(fd, imap, imap_size*HUST_BLOCKSIZE);
	if (res != imap_size*HUST_BLOCKSIZE) {
		perror("write_imap() erroe!");
		return -1;
	}
	return 0;
}

/* 
1. 写2个inode table（根文件夹和一个测试文件）信息到磁盘的inode table block
2. 写3个目录项数据到根文件夹的data block
*/
static int write_itable(int fd)
{
    uint32_t _uid = getuid();
    uint32_t _gid = getgid();
    
	ssize_t ret;
<<<<<<< HEAD
	// 1.定义根文件夹的inode，并写进磁盘
=======
	// 1.定义根文件夹的inode
>>>>>>> 0195dab93830a974322df9c20eea95bd24bc6c57
	struct HUST_inode root_dir_inode;
	root_dir_inode.mode = S_IFDIR;
	root_dir_inode.inode_no = HUST_ROOT_INODE_NUM;
	root_dir_inode.blocks = 1;
	root_dir_inode.block[0] = super_block.data_block_number;
	root_dir_inode.dir_children_count = 3;
    root_dir_inode.i_gid = _gid;
    root_dir_inode.i_uid = _uid;
    root_dir_inode.i_nlink = 2; 
    root_dir_inode.i_atime = root_dir_inode.i_mtime = root_dir_inode.i_ctime = ((int64_t)time(NULL));
    
	ret = write(fd, &root_dir_inode, sizeof(root_dir_inode));
	if (ret != sizeof(root_dir_inode)) {
		perror("write_itable error!\n");
		return -1;
	}
<<<<<<< HEAD
	// 2.定义onefile的inode信息，并写进磁盘
=======
	// 2.定义onefile的inode信息
>>>>>>> 0195dab93830a974322df9c20eea95bd24bc6c57
	struct HUST_inode onefile_inode;
	onefile_inode.mode = S_IFREG;
	onefile_inode.inode_no = 1;
	onefile_inode.blocks = 0;
	onefile_inode.block[0] = 0;
	onefile_inode.file_size = 0;
    onefile_inode.i_gid = _gid;
    onefile_inode.i_uid = _uid;
    onefile_inode.i_nlink = 1; 
    onefile_inode.i_atime = onefile_inode.i_mtime = onefile_inode.i_ctime = ((int64_t)time(NULL));

	ret = write(fd, &onefile_inode, sizeof(onefile_inode));
	if (ret != sizeof(onefile_inode)) {
		perror("write_itable error!\n");
		return -1;
	}


	// 3.定义根文件夹内的3个目录项
	struct HUST_dir_record root_dir_c;
	struct HUST_dir_record root_dir_p;
	struct HUST_dir_record file_record;
	const char* cur_dir = ".";
	const char* parent_dir = "..";
	const char* onefile = "file";
	memcpy(root_dir_c.filename, cur_dir, strlen(cur_dir) + 1);
	root_dir_c.inode_no = HUST_ROOT_INODE_NUM;
	memcpy(root_dir_p.filename, parent_dir, strlen(parent_dir) + 1);
	root_dir_p.inode_no = HUST_ROOT_INODE_NUM;
	memcpy(file_record.filename, onefile, strlen(onefile) + 1);
	file_record.inode_no = 1;

	off_t current_off = lseek(fd, 0L, SEEK_CUR);
	printf("Current seek is %lu and rootdir at %lu\n", current_off
			, super_block.data_block_number*HUST_BLOCKSIZE);

	// 4.定位文件指针到根文件夹的block处
	if(-1 == lseek(fd, super_block.data_block_number*HUST_BLOCKSIZE, SEEK_SET)) {
		perror("lseek error\n");
		return -1;
	}
<<<<<<< HEAD
	// 5. 将三个目录项写进磁盘（根文件夹的block内）。
=======
	// 5. 将三个目录项写进根文件夹的block内。
>>>>>>> 0195dab93830a974322df9c20eea95bd24bc6c57
	ret = write(fd, &root_dir_c, sizeof(root_dir_c));
	ret = write(fd, &root_dir_p, sizeof(root_dir_p));
	ret = write(fd, &file_record, sizeof(file_record));
	if (ret != sizeof(root_dir_c)) {
		perror("Write error!\n");
		return -1;
	}
	printf("Create root dir successfully!\n");
	return 0;
}


int main(int argc, char *argv[])
{
	int fd;
	ssize_t ret;

	if(argc != 2) {
		printf("Usage: mkfs <device>\n");
		return -1;
	}

	fd = open(argv[1]
			, O_RDWR);
	if (fd == -1) {
		perror("Error opening the device");
		return -1;
	}
	ret = 1;

	init_disk(fd, argv[1]);
	write_dummy(fd);
	write_sb(fd);
	write_bmap(fd);
	write_imap(fd);
	write_itable(fd);
//	write_root_dir(fd);
//	write_welcome_file(fd);

	close(fd);
	return ret;
}
