#ifndef INODE_H
#define INODE_H

// #include"segment.h"
#include<unistd.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_BLKS_FOR_FILE	500	// Maximum number of blocks a file  can occupy

struct direct_blk {
	int32_t seg_num;
	int32_t blk_num;
};
struct inode {
	uint32_t ino;	// inode number
	uint32_t size;
	struct direct_blk direct[MAX_BLKS_FOR_FILE];
};

void alloc_inode();
struct inode* read_inode();
int is_inode_free(struct inode *);

#endif
