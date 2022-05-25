#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
    assert(bpb->sec_per_clus == 1);

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




// Plug in your OS's favorite allocation scheme here.
static inode *ialloc() {
    return malloc(sizeof(inode));
}

static void idalloc(inode *in) {
    free(in);
}

// We alloc an inode everytime we get,
// but in reality we should check if
// we already have the requested inode in memory.
// and just return that with ref+1?
// and when we somehow release the inode,
// we decrease ref by 1, and when it reaches
// 0, we can recycle the inode.
static inode *iget(u32 dev, u32 inum) {
    inode *in = ialloc();
    in->inum = inum;

    return in;
}

// before the lazy me complete path resolution,
// I will provide this lazy function for getting
// the root dir inode, whose inum is, oh wait...
// root dir doesn't have a dir entry.
// 
// and when we want to search thourgh a dir's dir entries,
// we want to know the data region of that dir.
//
// for normal dirs other than root, we define the inum to
// be (clus_no << 12 | off_in_bytes) to indicate where the
// corresponding file's dir entry is. we encode the dir entry
// because we want to be able to do unlink() by simply
// setting the dir_entry[0] to be DE_FREED.
//
// but root dir does not have a directory entry.
// how do we find its data region if it doesn't have a dir entry?
// oh the superblock told us, so we can define inum=0 to be root 
// entry, and provide a helper function to convert inum to first
// data cluster number.
//
// all we do is to specially handle it.
static inode *get_root_inode() {
    return iget(0, /* inum */ 0);
}

#define CLUS2SEC(_CLUS_NO, _CLUS_OFF, _SEC_NO, _SEC_OFF) do {\
    _SEC_NO = ff->rootdir_base_sec + (_CLUS_NO - 2) * ff->bpb.sec_per_clus + _CLUS_OFF / BSIZE;\
    _SEC_OFF = _CLUS_OFF % BSIZE;\
} while (0)

typedef struct {
    u32 clus_no;
    u32 sec;
    u32 off; // offset in bytes
} fat_entry_loc;

fat_entry_loc get_fat_entry_location(u32 inum) {
    // we have ip->inum, which tells us where the dir entry is,
    // we can conclude from that where the fat entry is,
    // thus find the data region of the directory.
    //
    // this is 2 step memory or even disk indirection, thus extremely inefficient,
    // we can imporve it later

    // inode is root dir?
    fat_entry_loc loc;
    if (inum == 0) {
        loc.clus_no = 2;
        loc.sec = ff->bpb.rsvd_sec_cnt;
        loc.off = 8;
        return loc;
    }

    // location of dir entry
    u32 dir_clus = (inum >> 12);
    u32 dir_off_clus = (inum & 0xfff);

    u32 dir_sec, dir_off_sec;
    CLUS2SEC(dir_clus, dir_off_clus, dir_sec, dir_off_sec);
    u8 buf[BSIZE];
    bread(buf, dir_sec, 1);

    fat32_dirent *dent = (fat32_dirent *)(buf + dir_off_sec);
    
    u32 fat_clus = ((dent->fat_clus_hi << 16) + dent->fat_clus_lo);
    u32 fat_offset = fat_clus * 4;
    u32 fat_off_sec = ff->bpb.rsvd_sec_cnt + (fat_offset / BSIZE);
    u32 fat_entry_off = fat_offset % BSIZE;

    loc.clus_no = fat_clus;
    loc.sec = fat_off_sec;
    loc.off = fat_entry_off;
    return loc;

    /*
    bread(buf, fat_off_sec, 1);
    fat_entry fe = *((fat_entry *)(buf + fat_entry_off));
    */
}

static fat_entry read_fat_entry(fat_entry_loc loc) {
    u8 buf[BSIZE];
    bread(buf, loc.sec, 1);
    fat_entry fe = *((fat_entry *)(buf + loc.off));
    return fe;
}

static void scandir(inode *ip, scan_fn fn, void *res) {
    u32 fat_entry_sec, fat_entry_off;
    fat_entry_loc loc = get_fat_entry_location(ip->inum);

    fat_entry fe = read_fat_entry(loc);

    // We don't handle chain of clusters for now... FIXME PLEASE
    assert(fe == FE_EOC);

    // Now we arrive at the data region
    u32 sec;
    sec = ff->rootdir_base_sec + (loc.clus_no - 2) * ff->bpb.sec_per_clus;

    // Note that we assume in read_bpb sectors per cluster is one.
    // Haha, Lazy.
    u8 buf[BSIZE];
    bread(buf, sec, 1);
    fat32_dirent *dents = (fat32_dirent *)buf;
    for (int i = 0; i < BSIZE / sizeof(fat32_dirent); i++) {
        fat32_dirent *dent = dents + i;
        if ((u8)dent->name[0] == 0x00) {
            // Directory entry is free, and there's no dirents
            // following this entry anymore, stop getting dirents.
            break;
        }
        fn(loc.clus_no, i * sizeof(fat32_dirent), dent, res);
    }
}

void print_dirent(u32 clus_no, u32 offset, fat32_dirent *dent, void *res) {
    if ((u8)dent->name[0] == 0xe5) {
        // Directory entry is free, skip this one
        return;
    }

    int si = 0, di = 0;
    char name[12];
    while (si < 11) {
        char c = dent->name[si++];
        if (c == ' ') continue;
        if (c == 0x05) c = 0xe5; // Restore replaced DDEM character
        if (si == 9) name[di++] = '.';
        name[di++] = c;
    }
    printf("%s\n", name);
}

void test_ls() {
    scandir(get_root_inode(), print_dirent, 0);
}

// Returns non-zero if this file is a directory.
static inline int is_dirent_dir(fat32_dirent *dent) {
    return (dent->attr & ATTR_DIRECTORY);
}

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