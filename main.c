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
void test_bmap();
void test_balloc();
void test_readi();
void test_dirent_alloc();
void test_encode_sfn();
void test_dirlink();
void test_new_dir();
void test_truncate();

int main() {
    init_block_device();

    fat32 fs;
    init_fs(&fs);

    test_ls();
    printf("-----------------\n");
    test_open();
    printf("-----------------\n");
    test_bmap();
    printf("-----------------\n");
    test_readi();
    printf("-----------------\n");
    test_dirent_alloc();
    printf("-----------------\n");
    test_encode_sfn();
    printf("-----------------\n");
    test_dirlink();
    printf("-----------------\n");
    test_new_dir();
    printf("-----------------\n");
    test_truncate();

    release_block();
    return 0;
}