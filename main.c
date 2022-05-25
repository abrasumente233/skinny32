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

void test_ls();
void test_open();

int main() {
    init_block_device();

    fat32 fs;
    init_fs(&fs);

    test_ls();
    test_open();
    return 0;
}