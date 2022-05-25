#pragma once

typedef char       i8;
typedef short      i16;
typedef int        i32;
typedef long long  i64;

typedef i32        b32; // boolean

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef unsigned long long  usize;
typedef long long           isize;

/* Boringly bad interface implementation to the FAT32 filesystem. */
/* You can't find a worse version than mine. */
typedef struct __attribute__((__packed__)) fat32_bpb {
    u8  jmp_boot[3];
    u8  oem_name[8];
    u16 bytes_per_sec;
    u8  sec_per_clus;
    u16 rsvd_sec_cnt;
    u8  num_fats;
    u16 root_ent_cnt;
    u16 tot_sec_16;
    u8  media;
    u16 fat_sz_16;
    u16 sec_per_trk;
    u16 num_heads;
    u32 hidd_sec;
    u32 tot_sec_32;
    u32 fat_sz_32;
    u16 ext_flags;
    u16 fs_ver;
    u32 root_clus;
    u16 fs_info;
    u16 bk_boot_sec;
    u8  _reserved0[12];
    u8  drv_num;
    u8  _reserved1;
    u8  boot_sig;
    u32 vol_id;
    u8  vol_lab[11];
    u8  fil_sys_type[8];
} fat32_bpb;

typedef struct fat32 {
    fat32_bpb bpb;
    u32       rootdir_base_sec;
} fat32;

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME 0x0f

typedef struct __attribute__((__packed__)) fat32_dirent {
    char name[11];
    u8   attr;
    u8   ntres;
    u8   crt_time_tenth;
    u16  crt_time;
    u16  crt_date;
    u16  last_acc_date;
    u16  fst_clus_hi;
    u16  wrt_time;
    u16  wrt_date;
    u16  fst_clus_lo;
    u32  file_size;
} fat32_dirent;

// TODO: Support more types of files
#define DT_DIR 0x01 // Directory
#define DT_REG 0x02 // Regular file

// Funny "variable length" sturcture?
// @NOTE: No variable length struct for now, we only support FAT32
// and short name, so we reserve 8.3.1 (plus the terminator) chars.
typedef struct linux_dirent64 {
    u64  d_ino;           /* 64-bit inode number */
    u64  d_off;           /* 64-bit offset to next structure */
    u16  d_reclen;        /* Size of this dirent */
    u8   d_type;          /* File type: DT_DIR or DT_REG */
    char d_name[12];      /* Filename (null-terminated) */
} linux_dirent64;

void init_fs(fat32 *fs);
isize getdents(int fd, void *dirp, usize count);
