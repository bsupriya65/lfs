CC     = gcc
CFLAGS = -g -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26
LFLAGS = -lfuse
OBJS = src/inode.o src/lfs.o

all: lfs

lfs: $(OBJS)
	$(CC) $(OBJS) -o lfs $(LFLAGS)
	$(CC) test/lfs_test.c -o lfs_test

clean:
	rm -f lfs src/*.o lfs_test
	rm -f lfslog

# This is GNU makefile extension to notify that roughly means: 'clean' does
# not depend on any files in order to call it.
.PHONY: clean
