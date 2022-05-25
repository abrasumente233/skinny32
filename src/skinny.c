#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <block.h>
#include <skinny.h>

static fat32 *ff;

static void read_bpb(fat32 *fs) {
    fat32_bpb *bpb = &fs->bpb;

    // Lossly check some fields, the purpose is largely
    // to make sure we're dealing with a true FAT32 image.
    // @TODO: Support more valid FAT32 images.

    // @NOTE: Assume little endian loads here.
    assert(bpb->jmp_boot[0]   == 0xeb);
    assert(bpb->bytes_per_sec == 512);
    assert(bpb->num_fats      == 2);
    assert(bpb->root_ent_cnt  == 0);
    assert(bpb->tot_sec_16    == 0);
    assert(bpb->media         == 0xf8);
    assert(bpb->fat_sz_16     == 0);
    assert(bpb->fs_ver        == 0);
    assert(bpb->root_clus     == 2);
    assert(bpb->fs_info       == 1);
    assert(bpb->bk_boot_sec   == 6);

    // @TODO: Make some assertions about the drive/vol numbers.
}

void init_fs(fat32 *fs) {
    u8 boot_sector[BSIZE];
    ff = fs;
    bread(boot_sector, 0, 1);
    memcpy(&fs->bpb, boot_sector, sizeof(fat32_bpb));
    assert(boot_sector[510] == 0x55);
    assert(boot_sector[511] == 0xaa);

    read_bpb(fs);
    fat32_bpb *bpb = &fs->bpb;
    fs->rootdir_base_sec = bpb->rsvd_sec_cnt + bpb->fat_sz_32 * bpb->num_fats;

    printf("Sectors per cluster: %d\n", bpb->sec_per_clus);
    printf("Reserved sectors: %d\n", bpb->rsvd_sec_cnt);
    printf("FAT size: %d sectors\n", bpb->fat_sz_32);
    printf("Root cluster: %d\n", bpb->root_clus);
    printf("FAT 1 offset: 0x%x bytes\n", 512 * bpb->rsvd_sec_cnt);
    printf("FAT 2 offset: 0x%x bytes\n", 512 * (bpb->rsvd_sec_cnt + bpb->fat_sz_32));
    printf("Root directory offset: 0x%x bytes\n", 512 * fs->rootdir_base_sec);

    printf("sizeof(fat32_dirent) = %lu\n", sizeof(fat32_dirent));

    printf("FAT32 setup successfully\n");
}

// Returns non-zero if this file is a directory.
static inline int is_dirent_dir(fat32_dirent *dent) {
    return (dent->attr & ATTR_DIRECTORY);
}

/*
// Returns 1 if this file is a regular file.
static inline int is_dirent_reg(fat32_dirent *dent) {
    return !is_dirent_dir(dent);
}
*/

static void get_fileinfo(linux_dirent64 *ldent, fat32_dirent *dent) {
    ldent->d_ino = 44;
    ldent->d_off = 44;
    ldent->d_reclen = sizeof(linux_dirent64);
    ldent->d_type = is_dirent_dir(dent) ? DT_DIR : DT_REG;

    int si = 0, di = 0;
    while (si < 11) {
        char c = dent->name[si++];
        if (c == ' ') continue;
        if (c == 0x05) c = 0xe5; // Restore replaced DDEM character
        if (si == 9) ldent->d_name[di++] = '.';
        ldent->d_name[di++] = c;
    }
    ldent->d_name[di] = 0; // Terminate the SFN
}

/*
fat32_dirent find_dirent_by_name(const char *name) {
    fat32_dirent *dirent;
}

static int file_open(fat32_dir *dir, fat32_file *f, const char *name) {
    char buf[1024];
    
    // Cleanup: There's a lot of directory traversing code,
    // find a way to reuse it.
    while (1) {
        isize nread = getdents(-1, buf, 1024);
        if (nread == -1) break;
        for (long bpos = 0; bpos < nread;) {
            linux_dirent64 *d = (linux_dirent64 *)(buf + bpos);
            printf("%8llu  ", d->d_ino);
            u8 d_type = d->d_type;
            if (strcmp(name, d->d_name) == 0) {
                
            }
            bpos += d->d_reclen;
        }
    }

    return -1;
}
*/

// TODO: Make it able to read large directories
/*
isize getdents(int fd, void *dirp, usize count) {
    u32 sec = ff->rootdir_base_sec;
    fat32_dirent *dir_content = bread(sec);
    linux_dirent64 *ldirp = dirp;

    int read = 0;
    // FIXME: We only read one sector of dirents for now!
    for (int i = 0; read + sizeof(linux_dirent64) < count &&
                    i < 512 / sizeof(fat32_dirent);
         i++) {

        fat32_dirent *dent = &dir_content[i];

        if ((u8)dent->name[0] == 0xe5) {
            // Directory entry is free, skip this one
            continue;
        } else if ((u8)dent->name[0] == 0x00) {
            // Directory entry is free, and there's no dirents
            // following this entry anymore, stop getting dirents.
            break;
        } else {
            get_fileinfo(ldirp, dent);
            printf("d_name = %s\n", ldirp->d_name);
            ldirp++;
            read += sizeof(linux_dirent64);
        }
    }

    return read;
}
*/