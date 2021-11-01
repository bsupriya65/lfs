#include"lfs.h"
#include "inode.h"

#define FUSE_USE_VERSION	26

const char* expected_open = NULL;


// initialise all the global variables
void lfs_init()
{
	int i;
	li = (struct lfs_global_info*)malloc(sizeof(struct lfs_global_info));
	//initialise global variables
	li->cur_seg_buf    = malloc(SEG_SIZE);
	memset((void*)li->cur_seg_buf,0,SEG_SIZE);


	// indicate the presence of segment summary in first block by setting 
	// inode_num and logical_blk of segment summary entry as -1	
	struct segsum *ss = (struct segsum*)(li->cur_seg_buf);	
	ss[0].inode_num = -1;
	ss[0].logical_blk = -1;


	li->fih 	   = NULL;
	li->cur_seg_blk    = 1;
	li->log_head	   = 0;
	li->n_inode	   = 0;
	li->threshold	   = THRESHOLD;
	// allocate memory to bitmap and set all values as 1 indicating
	// all free segments initiallly
	for(i = 0; i< MAX_NUM_SEG; i++)
		li->seg_bitmap[i] = 1;	

	// initialize all the inode map entries to -1
	for(i=0;i<MAX_INODES;i++)
	{
		li->ino_map[i].seg_num = -1;
		li->ino_map[i].blk_num = -1;
	}
	// create a file of required size on disk that needs to be used to 
	// represent the lfs file system.	
	li->fd		 = open("./lfslog",O_RDWR|O_CREAT|O_TRUNC);
	assert(li->fd > 0);

	int file_size = (SEG_SIZE * MAX_NUM_SEG + BLKSIZE);
	char *buf = malloc(file_size);
	memset((void*)buf,0,file_size);
	pwrite(li->fd, buf, file_size,0);
	free(buf);
}

//get the file name from the path
const char* get_filename(const char *path)
{
        const char *p;
        p = path;

        while(*p != '\0') {
                p++;
        }

        while(*p != '/')
                p--;
        p++;
        return p;
}


static int lfs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	struct file_inode_hash *s;
	const char *p;

	p = get_filename(path);
	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		res = 0;
		return res;
	} else {
		for (s=li->fih; s!=NULL; s=s->hh.next) {
			if(!strcmp(p, s->f_name)) {		
				stbuf->st_mode = S_IFREG | 0755;
				stbuf->st_nlink = 1;
				res = 0;
				stbuf->st_size = s->f_size;
				return res;
			}
		}
	}
	res = -ENOENT;
	return res;
}
	
static int lfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;
	struct file_inode_hash *s;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	for (s=li->fih; s!=NULL; s=s->hh.next) {
		filler(buf, s->f_name, NULL, 0);
	}

	return 0;
}
// New file creation
int lfs_create(const char *path, mode_t mode,struct fuse_file_info *fi)
{
	expected_open = path;
	struct segsum *ss;
	int j;
	struct file_inode_hash *s;

	printf("\n inside the create func");


	HASH_FIND_STR(li->fih,get_filename(path),s);
	
	// if the file already exists, do nothing
	if(s != NULL)
		return 0;
	
	// if the hash returns null, then create new file
	else {
		struct inode *i = (struct inode *)(li->cur_seg_buf + (li->cur_seg_blk * BLKSIZE));

		// initialise the inode
		i->ino = li->n_inode++;
		i->size = 0;
		

		// add the newly created inode to for given file into the hash table
		s = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		strcpy(s->f_name,get_filename(path));
		s->inode_num = i->ino;
		s->f_size = 0;
		HASH_ADD_STR(li->fih,f_name,s);
		
		//initialise  all the direct blk values to zero
		for(j = 0; j <= MAX_BLKS_FOR_FILE; j++)
		{
			i->direct[j].seg_num = -1;
			i->direct[j].blk_num = -1;
		}

		
		//initialise the inode map
		li->ino_map[i->ino].seg_num = li->log_head;
		li->ino_map[i->ino].blk_num = li->cur_seg_blk; 
		
		//initialise the segment summary
		ss = (struct segsum *)(li->cur_seg_buf);
		ss[li->cur_seg_blk].inode_num = i->ino;
		ss[li->cur_seg_blk].logical_blk = -1;


		// if the in-memory segment is full, write the data to disk
                copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + BLKSIZE); 		
		
		li->cur_seg_blk++;
	}

	return 0;
}

// file open operation
int lfs_open(const char *path, struct fuse_file_info *fi) 
{

	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih,get_filename(path),s);
	
	if(strcmp(s->f_name,path) != 0)
		return 0;
	else
		return -1;

}

int lfs_unlink(const char *path)
{
	int ino;
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih,get_filename(path),s);
	
	if(s == NULL)
		return -1;

	ino = s->inode_num;
	li->ino_map[ino].seg_num = -1;
	li->ino_map[ino].blk_num = -1;

	HASH_DEL(li->fih,s);
	return 0;
}
	
int lfs_read(const char *path, char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	struct inode *i;
	uint32_t ino;
	int pos,n,blk;
	char *ibuf = malloc(BLKSIZE);

	// check if the file exists in hash table
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih,get_filename(path),s);

	// if given file is not present , return error	
	if( s == NULL)
		return -ENOENT;
	
	// store the inode number of the given file in ino variable
	else 
		ino = s->inode_num;
	
	memset(buf, 0, count);
	// handle the case where inode is not  in memory buffer
	if(li->ino_map[ino].seg_num != li->log_head)
	{
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
		 i = (struct inode *) ibuf;
	}
	
	else
		i = (struct inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE);

	if(offset  >=  i->size)
		return 0;

	count = MIN(count, i->size - offset);
	for(pos = offset; pos < offset + count; )
	{
		n = MIN(BLKSIZE - pos % BLKSIZE, offset + count - pos);
		blk = pos/BLKSIZE;
			
		/* if current  block is part of memory
		    move the n byes of data starting from  appropriate 
		    block into the buffer */
		if(i->direct[blk].seg_num == li->log_head)
			memmove(buf,  li->cur_seg_buf + i->direct[blk].blk_num * BLKSIZE+ pos % BLKSIZE, n);
		
		// if the current block is on disk, read the data from disk using read system call
		else
			read_from_log(i->direct[blk].seg_num, i->direct[blk].blk_num, buf, n, pos % BLKSIZE);

		pos += n;
		buf += n;
	}

	return count;
}

// get_next_free_segment returns the number of next free segment
// to which the global log_head is made to point to
int get_next_free_segment()
{
        int i,j;
	j = li->log_head + 1;
        for(i = 0; i < MAX_NUM_SEG ; i++,j++) 
        {
		if( j == MAX_NUM_SEG )
			j = 0;
                if(li->seg_bitmap[j] == 1)
                {
                        return j;
                }
        }
        return -1;
}

int num_of_free_segments()
{
	int i, count = 0;
	for(i = 0; i < MAX_NUM_SEG; i++)
	{
		if(li->seg_bitmap[i] == 1)
			count++;
	}
	return count;
}

void copy_segmentdata_to_log(int fd, char * buf, size_t count, off_t offset )
{
        size_t ret;
        struct segsum *ss = (struct segsum*)(li->cur_seg_buf);
        // if this is the last block in segment, write it into disc. 
        if( li->cur_seg_blk == MAX_SEG_BLKS -1)
        {
                ret = pwrite(fd, buf, count, offset);
                assert(ret == count);

                // update the bitmap indicating that the segment is not free
                li->seg_bitmap[li->log_head] = 0;

                li->log_head = get_next_free_segment();
		
		if(num_of_free_segments() <= 5)
		{
			li->cur_seg_blk = 1;
			lfs_clean();
		}

                li->cur_seg_blk = 0;

                // reset the memory buffer to zer0
                memset((void*)li->cur_seg_buf,0,SEG_SIZE);

                // resetting values for the segment summary entry for new segment
                ss[0].inode_num = -1;
                ss[0].logical_blk = -1;
        }
        return;
}


void read_from_log(int seg_num, int block_num, char *buf, int size, int blk_offset)
{
        int offset;
        offset = seg_num * SEG_SIZE + block_num * BLKSIZE + BLKSIZE + blk_offset;
        pread(li->fd, buf, size, offset);
        return;
}

// clean_cost returns the number of deadblocks present the segment
int clean_cost(int segment_num, char *ssbuf)
{
	int ino, blk, j, deadblock = 0; 
	struct inode *i;
	char *ibuf;
	ibuf = malloc(BLKSIZE);
	int segno = segment_num;

	read_from_log(segno, 0, ssbuf, BLKSIZE, 0);

	struct segsum *ss = (struct segsum *) ssbuf;
	for(j = 1; j < MAX_SEG_BLKS; j++)
	{
		ino = ss[j].inode_num;
                blk = ss[j].logical_blk;

		// represents its an inode
		if(blk == -1) 
		{
			//if segment number or block number in ifile dont match
			if((li->ino_map[ino].seg_num != segno) || (li->ino_map[ino].blk_num != j)) 
			{
				// mark as dead block
				ss[j].inode_num = -1;  
				deadblock++;
			}
		}
		else
		{
			read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
			i = (struct inode *) ibuf;
			
			// if block address in inode do not match
			if((i->direct[blk].seg_num != segno) || (i->direct[blk].blk_num != j)) 
			{
				// mark as a deadblock
				ss[j].inode_num = -1;  
				ss[j].logical_blk = -1;
				deadblock++;
			}
		}
	}
	free(ibuf);
	return deadblock;
}

void lfs_clean()
{

	char *new_buf = malloc(SEG_SIZE);
	char *ssbuf = malloc(BLKSIZE);
	int ino, blk, i, j, db;
	for(i =0; i < MAX_NUM_SEG; i++)
	{
       	    // if ith segment is not free
	    if(!li->seg_bitmap[i])
	    {
		db = clean_cost(i, ssbuf);
		if(db >= li->threshold)
		{
			// live blocks are to be copied into this segment	
			struct segsum *ss = (struct segsum *) li->cur_seg_buf; 
			read_from_log(i, 0, new_buf, SEG_SIZE, 0);

			// segment to be cleaned
			struct segsum *clean_ss = (struct segsum *) ssbuf ; 
			for(j = 1; j < MAX_SEG_BLKS; )
			{
				ino = clean_ss[j].inode_num;
				blk = clean_ss[j].logical_blk;

				// check if it is a deadblock
				if( ino != -1)  
				{
					// inode of the block not in memory
					if( li->ino_map[ino].seg_num != li->log_head) 
					{
						read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, BLKSIZE, 0);

						// update the inode map
						li->ino_map[ino].seg_num = li->log_head; 
						li->ino_map[ino].blk_num = li->cur_seg_blk;
					
						// update the segment summary
						ss[li->cur_seg_blk].inode_num = ino;  

						// mark as inode 
						ss[li->cur_seg_blk].logical_blk = -1; 
					
						copy_segmentdata_to_log(li->fd,li->cur_seg_buf, SEG_SIZE,li->log_head * SEG_SIZE + BLKSIZE);
						li->cur_seg_blk++;
					}
					// check if the current block in clean segment is an inode	
					if( blk == -1) 
					{
						j++;
						// as inode is copied, continue with the process
						continue;  
					}
					struct inode *id = (struct inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE); 

					// copy the block from new_buf into current segment.
					memmove(li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, new_buf + j*BLKSIZE, BLKSIZE); 
					// update the inode
					id->direct[blk].seg_num = li->log_head; 
					id->direct[blk].blk_num = li->cur_seg_blk;
					

					// update segment summary as 1 block used
					ss[li->cur_seg_blk].inode_num = ino; 
					ss[li->cur_seg_blk].logical_blk = blk; 
					copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,li->log_head * SEG_SIZE + BLKSIZE);
					li->cur_seg_blk++;
				}
				// consider next block in clean segment.
				j++;
			}	
			// Mark ith segment as free
			li->seg_bitmap[i] = 1;
		    }
		}
	}
	return;
}




int lfs_write(const char *path, const char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	char *ibuf = malloc(BLKSIZE);	
	struct inode *i;
	struct segsum *ss = (struct segsum *) li->cur_seg_buf;
	int pos,blk,segno;
	uint32_t ino, n;
	struct file_inode_hash *s,*s1;

	// check if the file exists in hash table
	HASH_FIND_STR(li->fih,get_filename(path),s);

	// if given file is not present , return error	
	if( s == NULL)
		return -ENOENT;
	
	// store the inode number of the given file in ino variable
	else 
		ino = s->inode_num;

	if(li->ino_map[ino].seg_num != li->log_head)  
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
	else
		memmove(ibuf,  li->cur_seg_buf  + li->ino_map[ino].blk_num * BLKSIZE, BLKSIZE);
	
	i = (struct inode *) ibuf;		
	for(pos = offset; pos < offset + count;)
	{
		// no. of bytes to be written in this block either whole block or few
		n = MIN(BLKSIZE - pos % BLKSIZE, offset + count - pos);
		blk = pos/BLKSIZE;

		if( pos + n > i->size)
			i->size = pos + n; 	// update file size accordingly. 

		 // disk address of the block to be written
		segno = i->direct[blk].seg_num;

		// this block already exists		
		if(segno != -1)
		{	
			// writing in continution of something already present
			if( pos % BLKSIZE != 0 )
			{
				if( segno == li->log_head) // block is still in memory
				{				
					memmove( li->cur_seg_buf + i->direct[blk].blk_num * BLKSIZE, buf, n); 
					pos += n; // update pos.
					buf += n;
					continue;
  				}
				else
					read_from_log(i->direct[blk].seg_num, i->direct[blk].blk_num, li->cur_seg_buf + li->cur_seg_blk*BLKSIZE, BLKSIZE, 0); 
			}
			
		}
		
		// update this block address in inode.
		i->direct[blk].seg_num  = li->log_head;
		i->direct[blk].blk_num = li->cur_seg_blk; 

		// write n bytes into memory buffer  from given buffer buf.
		memmove( li->cur_seg_buf + (li->cur_seg_blk ) * BLKSIZE, buf, n); 

		//update the summary
		ss[li->cur_seg_blk].inode_num = ino;
		ss[li->cur_seg_blk].logical_blk = blk; 

		 // if this is last block in segment write the whole segment into disk.
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, (li->log_head * SEG_SIZE + BLKSIZE));

		li->cur_seg_blk++; 		
		pos += n; // update pos.
		buf += n;
	}

	// If the inode to be updated is present in current memory segment
	// move the updated inode info directly to that block
	if(li->ino_map[ino].seg_num == li->log_head)
		memmove(li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE, ibuf, BLKSIZE);

	// If block containing indoe info was not part of current memory segment
	// copy inode info into current segment and update the summary
	else
	{
		memmove(li->cur_seg_buf + li->cur_seg_blk * BLKSIZE, ibuf, BLKSIZE);
		// update the inode map.
		li->ino_map[ino].seg_num = li->log_head; 
		li->ino_map[ino].blk_num = li->cur_seg_blk;

		//update segment summary for newly changes inode
		ss[li->cur_seg_blk].inode_num = ino;
		ss[li->cur_seg_blk].logical_blk = -1;

	// if this is the last block in segment, write it into disk. 
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,(li->log_head * SEG_SIZE + BLKSIZE));
		li->cur_seg_blk ++; 	
	}
	// update the hash information corresponding to the file
	//s->f_size = i->size;
        if(s->f_size != i->size) {
		s1 = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		s1->f_size = i->size;
		s1->inode_num = s->inode_num;
		strcpy(s1->f_name, s->f_name);
		HASH_DEL(li->fih,s);
		HASH_ADD_STR(li->fih,f_name,s1);
	}
	
	return count;
}


static struct fuse_operations lfs_oper = {
    .getattr	= lfs_getattr,
    .readdir	= lfs_readdir,
    .open	= lfs_open,
    .create	= lfs_create,
    .read	= lfs_read,
    .write	= lfs_write,
    .unlink     = lfs_unlink,
};

int main(int argc, char *argv[])
{
    lfs_init();
     
    return fuse_main(argc, argv, &lfs_oper, NULL);
}

