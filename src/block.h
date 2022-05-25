#pragma once

#define BSIZE 512

void init_block_device();

// Reads `len` sectors from `sector` into `buf`, starting at `offset`.
void bread(void *buf, int off, int len);

// Writes `len` sectors from `buf` into `sector` starting at `offset`.
void bwrite(void *buf, int off, int len);