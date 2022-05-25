#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <block.h>
#include <skinny.h>

int main() {
    init_block_device();

    fat32 fs;
    init_fs(&fs);

    return 0;
}