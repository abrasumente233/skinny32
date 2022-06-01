#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <block.h>
#include <skinny.h>

#define min(a, b) ((a) < (b) ? (a) : (b))

#define FOR_EACH_CLUS(__inum, __clus, __fat_ent, ...)                              \
    do {                                                                       \
        u32 first_clus = get_first_data_cluster(__inum);                       \
        if (first_clus == 0)                                                   \
            break;                                                             \
                                                                               \
        for (u32 __clus = first_clus, __fat_ent;;) {                               \
            __fat_ent = get_fat_entry(__clus);                                       \
            __VA_ARGS__                                                        \
            if (is_fat_entry_eoc(__fat_ent))                                       \
                break;                                                         \
            __clus = __fat_ent;                                                    \
        }                                                                      \
    } while (0)

// Must be used with a inode of type T_DIR
#define FOR_EACH_DIRENT(__inum, __clus, __fat_ent, __off, __dirent, ...)           \
    do {                                                                       \
        u8 buf[BSIZE];                                                         \
        FOR_EACH_CLUS(__inum, __clus, __fat_ent, {                                 \
            bread(buf, clus_data_sector(__clus), 1);                           \
            fat32_dirent *dents = (fat32_dirent *)buf;                         \
            for (u32 i = 0; i < BSIZE / sizeof(fat32_dirent); i++) {           \
                u32 __off = i * sizeof(fat32_dirent);                          \
                fat32_dirent *__dirent = &dents[i];                            \
                __VA_ARGS__                                                    \
            }                                                                  \
        });                                                                    \
    } while (0)


static fat32 *ff;

static u32 fat_dir_size(struct inode *ip);
static u32 fat_encode_sfn(void *res, const char *name);
void iupdate(struct inode *ip);

#define CLUS2SEC(_CLUS_NO, _CLUS_OFF, _SEC_NO, _SEC_OFF)                       \
    do {                                                                       \
        _SEC_NO = ff->rootdir_base_sec +                                       \
                  (_CLUS_NO - 2) * ff->bpb.sec_per_clus + _CLUS_OFF / BSIZE;   \
        _SEC_OFF = _CLUS_OFF % BSIZE;                                          \
    } while (0)

static u32 clus_data_sector(u32 clus_no) {
    return ff->rootdir_base_sec + (clus_no - 2) * ff->bpb.sec_per_clus;
}

static u32 sec_to_clus(u32 sec) {
    return (sec - ff->rootdir_base_sec) / ff->bpb.sec_per_clus + 2;
}

static fat_entry get_fat_entry(u32 clus_no) {
    u32 fat_offset = clus_no * 4;
    u32 fat_off_sec = ff->bpb.rsvd_sec_cnt + (fat_offset / BSIZE);
    u32 fat_entry_off = fat_offset % BSIZE;

    u8 buf[BSIZE];
    bread(buf, fat_off_sec, 1);

    fat_entry fe = *((fat_entry *)(buf + fat_entry_off));
    return fe;
}

// TODO: Performance stonks!
static void set_fat_entry(u32 clus_no, fat_entry entry) {
    u32 fat_offset = clus_no * 4;
    u32 fat_off_sec = ff->bpb.rsvd_sec_cnt + (fat_offset / BSIZE);
    u32 fat_entry_off = fat_offset % BSIZE;

    u8 buf[BSIZE];
    bread(buf, fat_off_sec, 1);

    *((fat_entry *)(buf + fat_entry_off)) = entry;

    bwrite(buf, fat_off_sec, 1);
}

static int is_fat_entry_eoc(fat_entry fe) {
    return fe >= 0x0ffffff8 && fe <= 0x0fffffff;
}

// Returns non-zero if this file is a directory.
static inline int is_dirent_dir(fat32_dirent *dent) {
    return (dent->attr & ATTR_DIRECTORY);
}

static void read_bpb(fat32 *fs) {
    fat32_bpb *bpb = &fs->bpb;

    // Lossly check some fields, the purpose is largely
    // to make sure we're dealing with a true FAT32 image.
    // @TODO: Support more valid FAT32 images.

    // @NOTE: Assume little endian loads here.
    assert(bpb->jmp_boot[0] == 0xeb);
    assert(bpb->bytes_per_sec == 512);
    assert(bpb->num_fats == 2);
    assert(bpb->root_ent_cnt == 0);
    assert(bpb->tot_sec_16 == 0);
    assert(bpb->media == 0xf8);
    assert(bpb->fat_sz_16 == 0);
    assert(bpb->fs_ver == 0);
    assert(bpb->root_clus == 2);
    assert(bpb->fs_info == 1);
    assert(bpb->bk_boot_sec == 6);
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
    printf("FAT 2 offset: 0x%x bytes\n",
           512 * (bpb->rsvd_sec_cnt + bpb->fat_sz_32));
    printf("Root directory offset: 0x%x bytes\n", 512 * fs->rootdir_base_sec);

    printf("sizeof(fat32_dirent) = %lu\n", sizeof(fat32_dirent));

    printf("FAT32 setup successfully\n");
}

static fat32_dirent read_fat32_dirent(u32 inum) {
    assert(inum != 0);

    u32 clus = inum >> 12;
    u32 off = inum & 0xfff;

    // location of dir entry
    u32 dir_clus = (inum >> 12);
    u32 dir_off_clus = (inum & 0xfff);

    u32 dir_sec, dir_off_sec;
    CLUS2SEC(dir_clus, dir_off_clus, dir_sec, dir_off_sec);
    u8 buf[BSIZE];
    bread(buf, dir_sec, 1);

    fat32_dirent *dent = (fat32_dirent *)(buf + dir_off_sec);

    return *dent;
}

static void write_fat32_dirent(u32 inum, fat32_dirent *dirent) {
    assert(inum != 0);

    u32 clus = inum >> 12;
    u32 off = inum & 0xfff;

    u32 sec = clus_data_sector(clus);

    char buf[BSIZE];
    bread(buf, sec, 1);
    fat32_dirent *d = (fat32_dirent *)(buf + off);

    *d = *dirent;
    bwrite(buf, sec, 1);
}

// Plug in your OS's favorite allocation scheme here.
static inode *ialloc() {
    inode *ip = malloc(sizeof(inode));
    memset(ip, 0, sizeof(inode));
    return ip;
}

static void idalloc(inode *in) { free(in); }

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
    in->parent = NULL;

    if (inum != 0) {
        fat32_dirent entry = read_fat32_dirent(inum);
        in->type = (entry.attr & ATTR_DIRECTORY) ? T_DIR : T_FILE;
        in->size = entry.file_size;
    } else {
        in->type = T_DIR;
    }

    if (in->type == T_DIR) {
        in->size = fat_dir_size(in);
    }

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
static inode *get_root_inode() { return iget(0, /* inum */ 0); }

// Returns the first data cluster number of the given inode.
static u32 get_first_data_cluster(u32 inum) {
    // we have ip->inum, which tells us where the dir entry is,
    // we can conclude from that where the fat entry is,
    // thus find the data region of the directory.
    //
    // this is 2 step memory or even disk indirection, thus extremely
    // inefficient, we can imporve it later

    // inode is root dir?
    if (inum == 0) {
        return 2;
    }

    fat32_dirent dent = read_fat32_dirent(inum);
    u32 fat_clus = ((dent.fat_clus_hi << 16) + dent.fat_clus_lo);

    return fat_clus;
}

// Allocate a new cluster and return its cluster number.
static u32 balloc() {
    // TODO: Optimize balloc using FSINFO
    // Go thourgh the FAT and find the first free cluster.

    // sizes are in the unit of sectors
    u32 fat_start = ff->bpb.rsvd_sec_cnt;
    u32 fat_sz = ff->bpb.fat_sz_32;

    for (u32 fat_sec = fat_start, clus_no = 0; fat_sec < fat_start + fat_sz;
         fat_sec++) {

        u8 buf[BSIZE];
        bread(buf, fat_sec, 1);

        for (u32 i = 0; i < BSIZE; i += 4, clus_no++) {
            if (clus_no < 2) {
                continue;
            }

            fat_entry fe = *((fat_entry *)(buf + i));
            if (fe == FE_FREE) {
                // found a free cluster
                // set it to EOC
                *((fat_entry *)(buf + i)) = 0x0fffffff;
                bwrite(buf, fat_sec, 1);
                return clus_no;
            }
        }
    }

    return 0; // No free clusters found
}

// Returns the sector number of the nth block
// in inode ip.
//
// If there's no such block, bmap allocates one.
static u32 bmap(inode *ip, u32 bn, int alloc) {

    assert(ip);

    u32 clus = get_first_data_cluster(ip->inum);

    if (clus == 0) {
        // I don't know upon regular file creation,
        // what the first data cluster is set to be.
        // So I just temproarily defaults it to 0.

        // TODO: Allocate a new cluster
        // And then the clus to be the newly allocated clus_no
        // whose fat entry is expected to be 0xffffffff
        // which is EOC
        if (alloc) {
            clus = balloc();
            printf("new clus: %d\n", clus);
            printf("clus sec: %d\n", clus_data_sector(clus));
            fat32_dirent dent = read_fat32_dirent(ip->inum);
            dent.fat_clus_lo = clus & 0xffff;
            dent.fat_clus_hi = (clus >> 16) & 0xffff;
            write_fat32_dirent(ip->inum, &dent);
        } else {
            return 0;
        }
    }

    int curr = 0;
    u32 res_clus = 0;
    FOR_EACH_CLUS(ip->inum, clus, fat_ent, {
        assert(fat_ent != FE_FREE);
        assert(fat_ent != FE_BAD);

        res_clus = clus;
        if (curr == bn) {
            break;
        }

        if (is_fat_entry_eoc(fat_ent)) {
            // allocate a new cluster
            // set the fat entry to be this clus
            // and assign it to clus.
            if (alloc) {
                u32 new_clus = balloc();
                set_fat_entry(clus, new_clus);
                
                // NOTE: Make the macro continue the loop
                fat_ent = new_clus;
            } else {
                return 0;
            }
        }
        curr += 1;
    });

    u32 sec = clus_data_sector(res_clus);

    // Cache the nth block -> sector mapping
    // ip->bn = bn;
    // ip->sec = asd;

    return sec;
}

static u32 bmap_noalloc(inode *ip, u32 bn) { return bmap(ip, bn, 0); }

static u32 bmap_alloc(inode *ip, u32 bn) { return bmap(ip, bn, 1); }

static u32 fat_dir_size(struct inode *ip) {
    assert(ip);
    assert(ip->type == T_DIR);

    u32 size = 0;

    FOR_EACH_CLUS(ip->inum, __clus, fat_ent, {
        assert(fat_ent != FE_FREE);
        assert(fat_ent != FE_BAD);
        size += BSIZE;
    });

    return size;
}

static void fat_decode_sfn(char *name, fat32_dirent *dent) {
    int si = 0, di = 0;
    while (si < 11) {
        char c = dent->name[si++];
        if (c == ' ')
            continue;
        if (c == 0x05)
            c = 0xe5; // Restore replaced DDEM character
        if (si == 9)
            name[di++] = '.';
        name[di++] = c;
    }
    name[di] = 0;
}

#define IsSeparator(c) ((c) == '/' || (c) == '\\')
#define IsUpper(c) ((c) >= 'A' && (c) <= 'Z')
#define IsLower(c) ((c) >= 'a' && (c) <= 'z')

#define DDEM 0xE5  /* Deleted directory entry mark set to DIR_Name[0] */
#define RDDEM 0x05 /* Replacement of the character collides with DDEM */
#define NSFLAG 11
#define NS_LOSS 0x01   /* Out of 8.3 format */
#define NS_LFN 0x02    /* Force to create LFN entry */
#define NS_LAST 0x04   /* Last segment */
#define NS_BODY 0x08   /* Lower case flag (body) */
#define NS_EXT 0x10    /* Lower case flag (ext) */
#define NS_DOT 0x20    /* Dot entry */
#define NS_NOLFN 0x40  /* Do not find LFN */
#define NS_NONAME 0x80 /* Not followed */

/* Test if the byte is DBC 1st byte */
static int dbc_1st(u8 c) { return 0; /* Always false */ }

/* Test if the byte is DBC 2nd byte */
static int dbc_2nd(u8 c) { return 0; /* Always false */ }

static u32 fat_encode_sfn(void *res, const char *name) {
    u8 c, d, *sfn;
    u32 ni, si, i;
    const char *p;

    /* Create file name in directory form */
    p = name;
    sfn = res;
    memset(sfn, ' ', 11);
    si = i = 0;
    ni = 8;
    for (;;) {
        c = (u32)p[si++]; /* Get a byte */
        if (c <= ' ')
            break;            /* Break if end of the path name */
        if (IsSeparator(c)) { /* Break if a separator is found */
            while (IsSeparator(p[si]))
                si++; /* Skip duplicated separator if exist */
            break;
        }
        if (c == '.' || i >= ni) { /* End of body or field overflow? */
            if (ni == 11 || c != '.')
                return -1;
            i = 8;
            ni = 11; /* Enter file extension field */
            continue;
        }
        if (c >= 0x80) { /* Is SBC extended character? */
            assert(0 && "SBC extended character not supported");
            // c = ExCvt[c & 0x7F]; /* To upper SBC extended character */
        }
        if (dbc_1st(c)) {     /* Check if it is a DBC 1st byte */
            d = (u32)p[si++]; /* Get 2nd byte */
            if (!dbc_2nd(d) || i >= ni - 1)
                return -1;
            sfn[i++] = c;
            sfn[i++] = d;
        } else { /* SBC */
            if (strchr("*+,:;<=>[]|\"\?\x7F", (int)c))
                return -1;
            if (IsLower(c))
                c -= 0x20; /* To upper */
            sfn[i++] = c;
        }
    }
    if (i == 0)
        return -1; /* Reject nul string */

    if (sfn[0] == DDEM)
        sfn[0] = RDDEM; /* If the first character collides with DDEM, replace it
                           with RDDEM */
    // sfn[NSFLAG] = (c <= ' ' || p[si] <= ' ')
    //? NS_LAST
    //: 0; /* Set last segment flag if end of the path */

    return 0;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int readi(struct inode *ip, int user_dst, void *dst, u32 off, u32 n,
          u32 *inum) {
    u32 tot, m;
    char buf[BSIZE];

    
    if(off > ip->size || off + n < off)
      return 0;
    if(off + n > ip->size)
      n = ip->size - off;
  
    u32 inum_written = 0;

    for (tot = 0; tot < n; tot += m, off += m, dst += m) {
        u32 sec = bmap_noalloc(ip, off / BSIZE);
        u32 clus = sec_to_clus(sec);
        bread(buf, sec, 1);
        if (inum_written == 0 && inum) {
            inum_written = 1;
            *inum = (clus << 12) | (off % BSIZE);
        }
        m = min(n - tot, BSIZE - off % BSIZE);
        memcpy(dst, buf + (off % BSIZE), m);
    }

    return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int writei(struct inode *ip, int user_src, void *src, u32 off, u32 n) {
    u32 tot, m;
    char buf[BSIZE];

    // FIXME: Check the bound
    if (off > ip->size || off + n < off)
        return -1;
    /*
    if(off + n > MAXFILE*BSIZE)
      return -1;
    */

    for (tot = 0; tot < n; tot += m, off += m, src += m) {
        u32 sec = bmap_alloc(ip, off / BSIZE);
        bread(buf, sec, 1);
        m = min(n - tot, BSIZE - off % BSIZE);
        memcpy(buf + (off % BSIZE), src, m);
        bwrite(buf, sec, 1);
    }

    // Round ip->size up to the nearest BSIZE
    // if ip->type == T_DIR
    if (off > ip->size) {
        if (ip->type == T_DIR) {
            u32 size = fat_dir_size(ip);
            ip->size = (size + BSIZE - 1) & ~(BSIZE - 1);
        } else {
            ip->size = off;
        }
    }

    // FIXME: Write back inode.
    // write the i-node back to disk even if the size didn't change
    // because the loop above might have called bmap() and added a new
    // block to ip->addrs[].
    iupdate(ip);

    return tot;
}

inode *fat_dirlookup(inode *dir, char *name) {
    assert(dir->type == T_DIR);

    FOR_EACH_DIRENT(dir->inum, clus, __fat_ent, off, dent, {
        u32 inum = (clus << 12) | off;
        u8 first_byte = dent->name[0];

        if (first_byte == 0xe5) {
            // Directory entry is free, skip this one
            continue;
        } else if (first_byte == 0x00) {
            // End of directory
            return NULL;
        }

        char filename[12];
        fat_decode_sfn(filename, dent);

        // printf("%s\n", filename);
        if (strcmp(filename, name) == 0) {
            // printf("Found!\n");
            return iget(0, inum);
        }
    });

    return NULL;
}

static u32 dirent_alloc(inode *ip) {
    assert(ip->type == T_DIR);

    u32 inum = 0;
    int i;

    FOR_EACH_DIRENT(ip->inum, clus, __fat_ent, off, dent, {
        printf("i = %d\n", i);
        inum = (clus << 12) | off;
        u8 first_byte = dent->name[0];

        if (first_byte == 0xe5 || first_byte == 0x00) {
            printf("Allocated %d\n", inum);
            
            // Performance: Don't use bmap here.
            // Since we already know the current cluster,
            // we don't need bmap to traverse the FAT again.
            if (dent->name[0] == 0x00 && (i + 1) % 8 == 0) {
                bmap_alloc(ip, ip->size / BSIZE + 1);
                fat32_dirent last_entry = {0};
                writei(ip, 0, &last_entry, ip->size, sizeof(fat32_dirent));
            }

            return inum;
        }

        i += 1;
    });

    assert(0 && "Panic");

    return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
    char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= DIRSIZ)
        memcpy(name, s, DIRSIZ);
    else {
        memcpy(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
    struct inode *ip, *next;

    if (*path == '/')
        ip = get_root_inode();
    else
        assert(0 && "namex: relative path not supported");
    // ip = idup(myproc()->cwd);

    while ((path = skipelem(path, name)) != 0) {
        // FIXME: Check if ip is a directory
        // if not, return NULL
        // if (ip->type != T_DIR) {
        // iunlockput(ip);
        // return 0;
        //}

        if (nameiparent && *path == '\0') {
            // Stop one level early.
            return ip;
        }

        if ((next = fat_dirlookup(ip, name)) == 0) {
            return 0;
        }
        next->parent = ip;
        ip = next;
    }
    if (nameiparent) {
        return 0;
    }
    return ip;
}

struct inode *namei(char *path) {
    char name[DIRSIZ];
    return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
    return namex(path, 1, name);
}

void test_open() {
    // inode *ip = fat_dirlookup(get_root_inode(), "README.TXT");
    // inode *ip = namei("/README.TXT");
    inode *ip = namei("/TEST_DIR/POEM.TXT");
    assert(ip);
    printf("size：%d\n", ip->size);

    // test dir size
    ip = namei("/TEST_DIR");
    assert(ip);
    printf("size：%d\n", ip->size);

    // test dir size
    ip = namei("/");
    assert(ip);
    printf("size：%d\n", ip->size);
}

void test_bmap() {
    inode *ip = get_root_inode();
    u32 first_clus = bmap_alloc(ip, 0);
    printf("first_clus = 0x%x\n", first_clus);

    u32 second_clus = bmap_alloc(ip, 1);
    printf("second_clus = 0x%x\n", second_clus);

    u32 third_clus = bmap_alloc(ip, 2);
    printf("third_clus = 0x%x\n", third_clus);
}

void test_balloc() {
    u32 clus_no = balloc();
    printf("balloc = 0x%x\n", clus_no);
}

void test_readi() {
    inode *ip = get_root_inode();

    char buf[sizeof(fat32_dirent)];
    u32 off = 0;

    for (int i = 0; i < BSIZE / sizeof(fat32_dirent); i++) {
        off = i * sizeof(fat32_dirent);
        readi(ip, 0, buf, off, sizeof(fat32_dirent), NULL);
        fat32_dirent *dent = (fat32_dirent *)buf;

        if ((u8)dent->name[0] == 0xe5) {
            // Directory entry is free, skip this one
            continue;
        } else if ((u8)dent->name[0] == 0x00) {
            // Directory entry is free, and there's no dirents
            // following this entry anymore, stop getting dirents.
            break;
        } else {
            char name[12];
            fat_decode_sfn(name, dent);
            printf("%s\n", name);
        }
    }
}

// Returns the inode of the newly created dir entry.
static inode *dirlink(inode *dir, char *name, u32 inode_type) {
    assert(dir->type == T_DIR);

    u32 inum = dirent_alloc(dir);

    fat32_dirent dirent = {.attr = ((inode_type == T_DIR) ? ATTR_DIRECTORY : 0),
                           .file_size = 0};

    fat_encode_sfn(dirent.name, name);

    write_fat32_dirent(inum, &dirent);

    inode *ip = iget(0, inum);

    if (inode_type == T_DIR) {
        // Add . and .. to the new directory
        // Create . directory entry
        u32 dot_clus = get_first_data_cluster(ip->inum);
        char name[11] = ".          ";
        fat32_dirent dot = {.attr = ATTR_DIRECTORY,
                            .file_size = 0,
                            .fat_clus_hi = (dot_clus >> 16) & 0xffff,
                            .fat_clus_lo = dot_clus & 0xffff};
        memcpy(dot.name, name, 11);
        writei(ip, 0, &dot, 0, sizeof(fat32_dirent));

        // Create .. directory entry
        u32 dotdot_clus = get_first_data_cluster(dir->inum);
        fat32_dirent dotdot = {.attr = ATTR_DIRECTORY,
                               .file_size = 0,
                               .fat_clus_hi = (dotdot_clus >> 16) & 0xffff,
                               .fat_clus_lo = dotdot_clus & 0xffff};
        memcpy(dotdot.name, name, 11);
        dotdot.name[1] = '.';
        writei(ip, 0, &dotdot, sizeof(fat32_dirent), sizeof(fat32_dirent));
    }

    return ip;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void iupdate(struct inode *ip) {
    if (ip->inum == 0) {
        return;
    }

    fat32_dirent dirent = read_fat32_dirent(ip->inum);
    dirent.attr = (ip->type == T_DIR) ? ATTR_DIRECTORY : 0;
    if (ip->type != T_DIR) {
        dirent.file_size = ip->size;
    }
    write_fat32_dirent(ip->inum, &dirent);
}

// Truncate inode(discard contents)
void itrunc(inode *ip) {
    // TODO: Implement itrunc for T_DIR
    assert(ip->type == T_FILE);

    fat32_dirent dirent = read_fat32_dirent(ip->inum);

    u32 clus = get_first_data_cluster(ip->inum);
    if (clus == 0) {
        // we have nothing to truncate
        return;
    }

    fat_entry freed_entry = 0x0fffffff;

    while (clus != 0) {
        u32 entry = get_fat_entry(clus);
        set_fat_entry(clus, freed_entry);
        assert(entry != FE_FREE);
        assert(entry != FE_BAD);
        if (is_fat_entry_eoc(entry)) {
            break;
        }
        clus = entry;
    }

    dirent.fat_clus_hi = dirent.fat_clus_lo = 0;
    write_fat32_dirent(ip->inum, &dirent);

    ip->size = 0;
    iupdate(ip);
}

void test_dirent_alloc() {
    inode *ip = get_root_inode();
    u32 inum = dirent_alloc(ip);
    assert(inum);
    printf("inum = 0x%x\n", inum);

    u32 clus = inum >> 12;
    u32 off = inum & 0xfff;

    printf("sector = %d\n", clus_data_sector(clus));
    printf("off = %d\n", off);
}


void test_encode_sfn() {
    char name[12];
    u32 ret = fat_encode_sfn(name, "hello.txt");
    assert(ret == 0);
    printf("name = %s\n", name);
}

void test_dirlink() {
    inode *ip = get_root_inode();
    inode *new_file = dirlink(ip, "NEW.TXT", T_FILE);
    assert(new_file);
    printf("new_file inum = 0x%x\n", new_file->inum);
    char data[] = "hello world\n";

    printf("writing to new file\n");
    writei(new_file, 0, data, 0, strlen(data));

    printf("new file size: %d\n", new_file->size);

    char buf[512];
    printf("reading from file\n");
    readi(new_file, 0, buf, 0, strlen(data), 0);
    buf[strlen(data)] = '\0';
    printf("read: %s\n", buf);
}

void test_new_dir() {
    inode *ip = get_root_inode();
    inode *new_dir = dirlink(ip, "NEW_DIR", T_DIR);
    assert(new_dir);

    printf("new_dir inum = 0x%x\n", new_dir->inum);
    printf("new_dir size: %d\n", new_dir->size);

    // Try create new file in new_dir
    inode *new_file = dirlink(new_dir, "HEY.TXT", T_FILE);
    char data[] = "Don't go gentle into that good night\n";
    printf("writing to new file\n");
    writei(new_file, 0, data, 0, strlen(data));

    printf("new file size: %d\n", new_file->size);

    char buf[512];
    printf("reading from file\n");
    readi(new_file, 0, buf, 0, strlen(data), 0);
    buf[strlen(data)] = '\0';
    printf("read: %s\n", buf);
}

void test_truncate() {
    printf("truncate test\n");
    inode *file = namei("/FILE8.TXT");
    printf("before: file size = %d\n", file->size);
    itrunc(file);
    printf(" after: file size = %d\n", file->size);
}

void test_for_each_clus() {
    printf("for each clus test\n");
    inode *file = namei("/FILE9.TXT");
    // inode *file = get_root_inode();
    if (file == NULL) {
        printf("file not found\n");
        return;
    }

    FOR_EACH_CLUS(file->inum, clus, ent, {
        printf("clus = 0x%x\n", clus); // Why
        printf("ent = 0x%x\n", ent);
        u32 sec = clus_data_sector(clus);
        printf("sec = 0x%x\n", sec * BSIZE);
    });
}

void test_ls() {
    inode *root = get_root_inode();
    assert(root != NULL);

    FOR_EACH_DIRENT(root->inum, clus, ent, off, dirent, {
        u8 first_byte = dirent->name[0];
        if (first_byte == 0xe5) {
            continue;
        } else if (first_byte == 0x00) {
            break;
        }
        char name[12];
        fat_decode_sfn(name, dirent);
        printf("%s\n", name);
    });
}