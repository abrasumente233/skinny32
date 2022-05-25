#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <block.c>


int main() {
    init_block_device();

    char buf[BSIZE];
    bread(buf, 0, 1);

    printf("%x\n", buf[0] & 0xff);

    return 0;
}