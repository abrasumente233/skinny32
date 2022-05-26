#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <block.h>
#include <skinny.h>

#define min(a, b) ((a) < (b) ? (a) : (b))

static fat32 *ff;

static u32 get_dir_size(struct inode *ip);

#define CLUS2SEC(_CLUS_NO, _CLUS_OFF, _SEC_NO, _SEC_OFF)                       \
    do {                                                                       \
        _SEC_NO = ff->rootdir_base_sec +                                       \
                  (_CLUS_NO - 2) * ff->bpb.sec_per_clus + _CLUS_OFF / BSIZE;   \
        _SEC_OFF = _CLUS_OFF % BSIZE;                                          \
    } while (0)


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

// Plug in your OS's favorite allocation scheme here.
static inode *ialloc() { return malloc(sizeof(inode)); }

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
    
    if (inum != 0)  {
        fat32_dirent entry = read_fat32_dirent(inum);
        in->type = (entry.attr & ATTR_DIRECTORY) ? T_DIR : T_FILE;
        in->size = entry.file_size;
    } else {
        in->type = T_DIR;
    }

    if (in->type == T_DIR) {
        in->size = get_dir_size(in);
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

static u32 clus_data_sector(u32 clus_no) {
    return ff->rootdir_base_sec + (clus_no - 2) * ff->bpb.sec_per_clus;
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
        } else {
            return 0;
        }
    }

    while (bn--) {
        u32 entry = get_fat_entry(clus);
        assert(entry != FE_FREE);
        assert(entry != FE_BAD);
        if (is_fat_entry_eoc(entry)) {
            // allocate a new cluster
            // set the fat entry to be this clus
            // and assign it to clus.
            if (alloc) {
                u32 new_clus = balloc();
                set_fat_entry(clus, new_clus);
                clus = new_clus;
            } else {
                return 0;
            }
        } else { // is a valid cluster in the middle of chain
            clus = entry;
        }
    }

    u32 sec = clus_data_sector(clus);

    // Cache the nth block -> sector mapping
    // ip->bn = bn;
    // ip->sec = asd;

    return sec;
}

static u32 bmap_noalloc(inode *ip, u32 bn) {
    return bmap(ip, bn, 0);
}

static u32 bmap_alloc(inode *ip, u32 bn) {
    return bmap(ip, bn, 1);
}

static u32 get_dir_size(struct inode *ip) {
    assert(ip);
    assert(ip->type == T_DIR);

    u32 clus = get_first_data_cluster(ip->inum);
    u32 size = 0;

    while (clus != 0) {
        u32 entry = get_fat_entry(clus);
        assert(entry != FE_FREE);
        assert(entry != FE_BAD);
        size += BSIZE;
        clus = entry;
        if (is_fat_entry_eoc(entry)) {
            break;
        }
    }

    return size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int readi(struct inode *ip, int user_dst, void *dst, u32 off, u32 n) {
    u32 tot, m;
    char buf[BSIZE];

    /*
    if(off > ip->size || off + n < off)
      return 0;
    if(off + n > ip->size)
      n = ip->size - off;
  */

    for (tot = 0; tot < n; tot += m, off += m, dst += m) {
        bread(buf, bmap_noalloc(ip, off / BSIZE), 1);
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
int
writei(struct inode *ip, int user_src, void *src, u32 off, u32 n)
{
  u32 tot, m;
  char buf[BSIZE];

  // FIXME: Check the bound
  /*
  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;
  */

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
      
    bread(buf, bmap_noalloc(ip, off / BSIZE), 1);
    m = min(n - tot, BSIZE - off%BSIZE);
    memcpy(buf + (off % BSIZE), src, m);
  }

  // FIXME: Set inode's size
  /*
  if(off > ip->size)
    ip->size = off;
  */

  // FIXME: Write back inode.
  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  //iupdate(ip);

  return tot;
}


// Returns 0 if clus_no is a EOC.
// Returns a valid cluster number if not
// Oh if the ent is 0... you wont get a zero if you're a good citizen
static u32 next_in_chain(u32 clus_no) {
    fat_entry ent = get_fat_entry(clus_no);
    assert(ent != 0);

    if (is_fat_entry_eoc(ent)) {
        return 0;
    } else {
        return ent;
    }
}

static void scandir(inode *ip, scan_fn fn, void *res) {
    u32 fat_entry_sec, fat_entry_off;
    u32 first_clus = get_first_data_cluster(ip->inum);

    // Now we arrive at the data region
    for (u32 clus = first_clus; clus; clus = next_in_chain(clus)) {
        u32 sec = ff->rootdir_base_sec + (clus - 2) * ff->bpb.sec_per_clus;

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
                goto scan_done;
            }
            int scan_res = fn(clus, i * sizeof(fat32_dirent), dent, res);
            if (scan_res == SCAN_BREAK) {
                goto scan_done;
            }
        }
    }
scan_done:;
}

typedef struct {
    char *name;
    u32 clus_no;
    u32 off;
    fat32_dirent dent;
} search_result;

static void decode_fat_sfn(char *name, fat32_dirent *dent) {
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

// FIXME: Early bail-out
static int search_file(u32 clus_no, u32 offset, fat32_dirent *dent, void *res) {
    assert(res);
    search_result *sr = (search_result *)res;

    if ((u8)dent->name[0] == 0xe5) {
        // Directory entry is free, skip this one
        return SCAN_CONT;
    }

    int si = 0, di = 0;
    char name[12];
    decode_fat_sfn(name, dent);

    printf("name: %s, sr->name: %s\n", name, sr->name);
    if (strcmp(name, sr->name) == 0) {
        printf("Found!\n");
        sr->clus_no = clus_no;
        sr->off = offset;
        sr->dent = *dent;
        return SCAN_BREAK;
    }

    return SCAN_CONT;
}

inode *fat_dirlookup(inode *dir, char *name) {
    search_result res = {.name = name};
    scandir(dir, search_file, &res);

    // Found
    if (res.clus_no == 0) {
        return NULL;
    }

    u32 inum = (res.clus_no << 12) + res.off;
    inode *ip = iget(0, inum);

    return ip;
}

// Returns non-zero if this file is a directory.
static inline int is_dirent_dir(fat32_dirent *dent) {
    return (dent->attr & ATTR_DIRECTORY);
}

/*
static void get_fileinfo(linux_dirent64 *ldent, fat32_dirent *dent) {
    ldent->d_ino = 44;
    ldent->d_off = 44;
    ldent->d_reclen = sizeof(linux_dirent64);
    ldent->d_type = is_dirent_dir(dent) ? DT_DIR : DT_REG;

    int si = 0, di = 0;
    while (si < 11) {
        char c = dent->name[si++];
        if (c == ' ')
            continue;
        if (c == 0x05)
            c = 0xe5; // Restore replaced DDEM character
        if (si == 9)
            ldent->d_name[di++] = '.';
        ldent->d_name[di++] = c;
    }
    ldent->d_name[di] = 0; // Terminate the SFN
}
*/

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

static int print_dirent(u32 clus_no, u32 offset, fat32_dirent *dent,
                        void *res) {
    if ((u8)dent->name[0] == 0xe5) {
        // Directory entry is free, skip this one
        return SCAN_CONT;
    }

    char name[12];
    decode_fat_sfn(name, dent);
    printf("%s\n", name);

    return SCAN_CONT;
}

void test_ls() { scandir(get_root_inode(), print_dirent, 0); }

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
        readi(ip, 0, buf, off, sizeof(fat32_dirent));
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
            decode_fat_sfn(name, dent);
            printf("%s\n", name);
        }
    }
}