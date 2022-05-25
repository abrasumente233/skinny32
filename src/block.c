#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <block.h>

static void *drive;

void init_block_device() {
    int f;
    struct stat statbuf;

    f = openat(AT_FDCWD, "fs.img", O_RDWR);
    assert(f != 0);
    assert(fstat(f, &statbuf) == 0);
    drive = mmap(0, statbuf.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, f, 0);
    assert(drive != (void *)-1);
}

// Reads `len` sectors from `sector` into `buf`, starting at `offset`.
void bread(void *buf, int off, int len) {
    memcpy(buf, drive + off * BSIZE, len * BSIZE);
}

// Writes `len` sectors from `buf` into `sector` starting at `offset`.
void bwrite(void *buf, int off, int len) {
    memcpy(drive + off * BSIZE, buf, len * BSIZE);
}