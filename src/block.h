#pragma once

#define BSIZE 512

void init_block_device();
void release_block();

// Reads `len` sectors from `sector` into `buf`, starting at `offset`.
void bread(void *buf, int off, int len);

// Writes `len` sectors from `buf` into `sector` starting at `offset`.
void bwrite(void *buf, int off, int len);

void debug_print_block(unsigned char *buf);