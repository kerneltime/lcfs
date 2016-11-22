#include "includes.h"

/* Allocate a new file system structure */
struct fs *
lc_newFs(struct gfs *gfs, bool rw) {
    struct fs *fs = malloc(sizeof(struct fs));
    time_t t;

    t = time(NULL);
    memset(fs, 0, sizeof(*fs));
    fs->fs_gfs = gfs;
    fs->fs_readOnly = !rw;
    fs->fs_ctime = t;
    fs->fs_atime = t;
    pthread_mutex_init(&fs->fs_plock, NULL);
    pthread_mutex_init(&fs->fs_alock, NULL);
    pthread_rwlock_init(&fs->fs_rwlock, NULL);
    fs->fs_icache = lc_icache_init();
    fs->fs_stats = lc_statsNew();
    __sync_add_and_fetch(&gfs->gfs_count, 1);
    return fs;
}

/* Flush inode block map pages */
void
lc_flushInodeBlocks(struct gfs *gfs, struct fs *fs) {
    uint64_t count, block, pcount = fs->fs_inodeBlockCount;
    struct page *page, *fpage;
    struct iblock *iblock;

    if (pcount == 0) {
        return;
    }
    if (fs->fs_inodeBlocks != NULL) {
        fs->fs_inodeBlockPages = lc_getPageNoBlock(gfs, fs,
                                                   (char *)fs->fs_inodeBlocks,
                                                   fs->fs_inodeBlockPages);
        fs->fs_inodeBlocks = NULL;
    }
    block = lc_blockAlloc(fs, pcount, true);
    fpage = fs->fs_inodeBlockPages;
    page = fpage;
    count = pcount;
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        iblock = (struct iblock *)page->p_data;
        iblock->ib_next = (page == fpage) ?
                          fs->fs_super->sb_inodeBlock : block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
    fs->fs_inodeBlockCount = 0;
    fs->fs_inodeBlockPages = NULL;
    fs->fs_super->sb_inodeBlock = block;
}

/* Allocate a new inode block */
void
lc_newInodeBlock(struct gfs *gfs, struct fs *fs) {
    if (fs->fs_inodeBlockCount >= LC_CLUSTER_SIZE) {
        lc_flushInodeBlocks(gfs, fs);
    }
    if (fs->fs_inodeBlocks != NULL) {
        fs->fs_inodeBlockPages = lc_getPageNoBlock(gfs, fs,
                                                   (char *)fs->fs_inodeBlocks,
                                                   fs->fs_inodeBlockPages);
    }
    posix_memalign((void **)&fs->fs_inodeBlocks, LC_BLOCK_SIZE, LC_BLOCK_SIZE);
    memset(fs->fs_inodeBlocks, 0, LC_BLOCK_SIZE);
    fs->fs_inodeIndex = 0;
    fs->fs_inodeBlockCount++;
}

/* Delete a file system */
void
lc_destroyFs(struct fs *fs, bool remove) {
    struct gfs *gfs = fs->fs_gfs;

    lc_displayStats(fs);
    assert(fs->fs_blockInodesCount == 0);
    assert(fs->fs_blockMetaCount == 0);
    assert(fs->fs_dpcount == 0);
    assert(fs->fs_dpages == NULL);
    assert(fs->fs_inodePagesCount == 0);
    assert(fs->fs_inodePages == NULL);
    assert(fs->fs_inodeBlockCount == 0);
    assert(fs->fs_inodeBlockPages == NULL);
    assert(fs->fs_inodeBlocks == NULL);
    assert(fs->fs_extents == NULL);
    assert(fs->fs_aextents == NULL);
    assert(fs->fs_fextents == NULL);
    lc_destroyInodes(fs, remove);
    if (fs->fs_pcache && (fs->fs_parent == NULL)) {
        lc_destroy_pages(gfs, fs->fs_pcache, remove);
    }
    if (fs->fs_mextents) {
        lc_processFreedMetaBlocks(fs);
    }
    assert(fs->fs_mextents == NULL);
    if (fs->fs_ilock && (fs->fs_parent == NULL)) {
        pthread_mutex_destroy(fs->fs_ilock);
        free(fs->fs_ilock);
    }
    pthread_mutex_destroy(&fs->fs_plock);
    pthread_mutex_destroy(&fs->fs_alock);
    pthread_rwlock_destroy(&fs->fs_rwlock);
    lc_statsDeinit(fs);
    assert(fs->fs_icount == 0);
    assert(fs->fs_pcount == 0);
    __sync_sub_and_fetch(&gfs->gfs_count, 1);
    if (fs != lc_getGlobalFs(gfs)) {
        free(fs->fs_super);
        free(fs);
    }
    lc_printf("lc_destroyFs: fs %p, blocks allocated %ld freed %ld\n",
              fs, fs->fs_blocks, fs->fs_freed);
}

/* Lock a file system in shared while starting a request.
 * File system is locked in exclusive mode while taking/deleting snapshots.
 */
void
lc_lock(struct fs *fs, bool exclusive) {
    if (exclusive) {
        pthread_rwlock_wrlock(&fs->fs_rwlock);
    } else {
        pthread_rwlock_rdlock(&fs->fs_rwlock);
    }
}

/* Unlock the file system */
void
lc_unlock(struct fs *fs) {
    pthread_rwlock_unlock(&fs->fs_rwlock);
}

/* Check if the specified inode is a root of a file system and if so, return
 * the index of the new file system. Otherwise, return the index of current
 * file system.
 */
int
lc_getIndex(struct fs *nfs, ino_t parent, ino_t ino) {
    struct gfs *gfs = nfs->fs_gfs;
    int i, gindex = nfs->fs_gindex;
    ino_t root;

    /* Snapshots are allowed in one directory right now */
    if ((gindex == 0) && gfs->gfs_scount && (parent == gfs->gfs_snap_root)) {
        root = lc_getInodeHandle(ino);
        assert(lc_globalRoot(ino));
        for (i = 1; i <= gfs->gfs_scount; i++) {
            if (gfs->gfs_roots[i] == root) {
                return i;
            }
        }
    }
    return gindex;
}

/* Return the file system in which the inode belongs to */
struct fs *
lc_getfs(ino_t ino, bool exclusive) {
    int gindex = lc_getFsHandle(ino);
    struct gfs *gfs = getfs();
    struct fs *fs;

    assert(gindex < LC_MAX);
    fs = gfs->gfs_fs[gindex];
    lc_lock(fs, exclusive);
    assert(fs->fs_gindex == gindex);
    assert(gfs->gfs_roots[gindex] == fs->fs_root);
    return fs;
}

/* Add a file system to global list of file systems */
void
lc_addfs(struct fs *fs, struct fs *pfs, struct fs *snap) {
    struct gfs *gfs = fs->fs_gfs;
    int i;

    /* Find a free slot and insert the new file system */
    pthread_mutex_lock(&gfs->gfs_lock);
    for (i = 1; i < LC_MAX; i++) {
        if (gfs->gfs_fs[i] == NULL) {
            fs->fs_gindex = i;
            fs->fs_super->sb_index = i;
            gfs->gfs_fs[i] = fs;
            gfs->gfs_roots[i] = fs->fs_root;
            if (i > gfs->gfs_scount) {
                gfs->gfs_scount = i;
            }
            break;
        }
    }
    assert(i < LC_MAX);
    fs->fs_sblock = lc_blockAlloc(fs, 1, true);

    /* Add this file system to the snapshot list or root file systems list */
    if (snap) {
        fs->fs_next = snap->fs_next;
        snap->fs_next = fs;
        fs->fs_super->sb_nextSnap = snap->fs_super->sb_nextSnap;
        snap->fs_super->sb_nextSnap = fs->fs_sblock;
        snap->fs_super->sb_flags |= LC_SUPER_DIRTY;
    } else if (pfs) {
        pfs->fs_snap = fs;
        pfs->fs_super->sb_childSnap = fs->fs_sblock;
        pfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Remove a file system from the global list */
void
lc_removefs(struct gfs *gfs, struct fs *fs) {
    assert(fs->fs_snap == NULL);
    assert(fs->fs_gindex > 0);
    assert(fs->fs_gindex < LC_MAX);
    assert(gfs->gfs_fs[fs->fs_gindex] == fs);
    pthread_mutex_lock(&gfs->gfs_lock);
    gfs->gfs_fs[fs->fs_gindex] = NULL;
    gfs->gfs_roots[fs->fs_gindex] = 0;
    if (gfs->gfs_scount == fs->fs_gindex) {
        assert(gfs->gfs_scount > 0);
        gfs->gfs_scount--;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    fs->fs_gindex = -1;
}

/* Remove the file system from the snapshot list */
void
lc_removeSnap(struct gfs *gfs, struct fs *fs) {
    struct fs *pfs, *nfs;

    assert(fs->fs_snap == NULL);
    assert(fs->fs_gindex > 0);
    assert(fs->fs_gindex < LC_MAX);
    pthread_mutex_lock(&gfs->gfs_lock);
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {

        /* Parent points to this layer */
        pfs->fs_snap = fs->fs_next;
        pfs->fs_super->sb_childSnap = fs->fs_super->sb_nextSnap;
        pfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    } else {

        /* Remove from the common parent list */
        nfs = pfs ? pfs->fs_snap : lc_getGlobalFs(gfs);
        while (nfs) {
            if (nfs->fs_next == fs) {
                nfs->fs_next = fs->fs_next;
                nfs->fs_super->sb_nextSnap = fs->fs_super->sb_nextSnap;
                nfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
                break;
            }
            nfs = nfs->fs_next;
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Format a file system by initializing its super block */
static void
lc_format(struct gfs *gfs, struct fs *fs, size_t size) {
    lc_superInit(gfs->gfs_super, size, true);
    lc_rootInit(fs, fs->fs_root);
}

/* Allocate global file system */
static struct gfs *
lc_gfsAlloc(int fd) {
    struct gfs *gfs = malloc(sizeof(struct gfs));

    memset(gfs, 0, sizeof(struct gfs));
    gfs->gfs_fs = malloc(sizeof(struct fs *) * LC_MAX);
    memset(gfs->gfs_fs, 0, sizeof(struct fs *) * LC_MAX);
    gfs->gfs_roots = malloc(sizeof(ino_t) * LC_MAX);
    memset(gfs->gfs_roots, 0, sizeof(ino_t) * LC_MAX);
    pthread_mutex_init(&gfs->gfs_lock, NULL);
    pthread_mutex_init(&gfs->gfs_alock, NULL);
    gfs->gfs_fd = fd;
    return gfs;
}

/* Initialize a file system after reading its super block */
static struct fs *
lc_initfs(struct gfs *gfs, struct fs *pfs, uint64_t block, bool child) {
    struct super *super = lc_superRead(gfs, block);
    struct fs *fs;
    int i;

    fs = lc_newFs(gfs, super->sb_flags & LC_SUPER_RDWR);
    fs->fs_sblock = block;
    fs->fs_super = super;
    fs->fs_root = fs->fs_super->sb_root;
    if (child) {

        /* First child layer of the parent */
        assert(pfs->fs_snap == NULL);
        pfs->fs_snap = fs;
        fs->fs_parent = pfs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_ilock = pfs->fs_ilock;
    } else if (pfs->fs_parent == NULL) {

        /* Base layer */
        assert(pfs->fs_next == NULL);
        pfs->fs_next = fs;
        fs->fs_pcache = lc_pcache_init();
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
    } else {

        /* Layer with common parent */
        assert(pfs->fs_next == NULL);
        pfs->fs_next = fs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_parent = pfs->fs_parent;
        fs->fs_ilock = pfs->fs_ilock;
    }

    /* Add the layer to the global list */
    i = fs->fs_super->sb_index;
    assert(i < LC_MAX);
    assert(gfs->gfs_fs[i] == NULL);
    gfs->gfs_fs[i] = fs;
    gfs->gfs_roots[i] = fs->fs_root;
    if (i > gfs->gfs_scount) {
        gfs->gfs_scount = i;
    }
    fs->fs_gindex = i;
    lc_printf("Added fs with parent %ld root %ld index %d block %ld\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, block);
    return fs;
}

/* Initialize all file systems from disk */
static void
lc_initSnapshots(struct gfs *gfs, struct fs *pfs) {
    struct fs *fs, *nfs = pfs;
    uint64_t block;

    /* Initialize all snapshots of the same parent */
    block = pfs->fs_super->sb_nextSnap;
    while (block) {
        fs = lc_initfs(gfs, nfs, block, false);
        nfs = fs;
        block = fs->fs_super->sb_nextSnap;
    }

    /* Now initialize all the child snapshots */
    nfs = pfs;
    while (nfs) {
        block = nfs->fs_super->sb_childSnap;
        if (block) {
            fs = lc_initfs(gfs, nfs, block, true);
            lc_initSnapshots(gfs, fs);
        }
        nfs = nfs->fs_next;
    }
}

/* Set up some special inodes on restart */
static void
lc_setupSpecialInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *dir = fs->fs_rootInode;
    ino_t ino;

    ino = lc_dirLookup(fs, dir, "lcfs");
    if (ino != LC_INVALID_INODE) {
        gfs->gfs_snap_rootInode = lc_getInode(lc_getGlobalFs(gfs), ino,
                                               NULL, false, false);
        if (gfs->gfs_snap_rootInode) {
            lc_inodeUnlock(gfs->gfs_snap_rootInode);
        }
        gfs->gfs_snap_root = ino;
        printf("snapshot root %ld\n", ino);
    }
}

/* Mount the device */
int
lc_mount(char *device, struct gfs **gfsp) {
    struct gfs *gfs;
    struct fs *fs;
    size_t size;
    int fd, err;
    int i;

    /* Open the device for mounting */
    fd = open(device, O_RDWR | O_DIRECT | O_EXCL | O_NOATIME, 0);
    if (fd == -1) {
        perror("open");
        return errno;
    }

    /* Find the size of the device and calculate total blocks */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror("lseek");
        return errno;
    }
    gfs = lc_gfsAlloc(fd);

    /* Initialize a file system structure in memory */
    /* XXX Recreate file system after abnormal shutdown for now */
    fs = lc_newFs(gfs, true);
    fs->fs_root = LC_ROOT_INODE;
    fs->fs_sblock = LC_SUPER_BLOCK;
    fs->fs_pcache = lc_pcache_init();
    gfs->gfs_fs[0] = fs;
    gfs->gfs_roots[0] = LC_ROOT_INODE;

    /* Try to find a valid superblock, if not found, format the device */
    fs->fs_super = lc_superRead(gfs, fs->fs_sblock);
    gfs->gfs_super = fs->fs_super;
    if ((gfs->gfs_super->sb_magic != LC_SUPER_MAGIC) ||
        (gfs->gfs_super->sb_version != LC_VERSION) ||
        (gfs->gfs_super->sb_flags & LC_SUPER_DIRTY)) {
        printf("Formating %s, size %ld\n", device, size);
        lc_format(gfs, fs, size);
    } else {
        if (gfs->gfs_super->sb_flags & LC_SUPER_DIRTY) {
            printf("Filesystem is dirty\n");
            return EIO;
        }
        assert(size == (gfs->gfs_super->sb_tblocks * LC_BLOCK_SIZE));
        gfs->gfs_super->sb_mounts++;
        printf("Mounting %s, size %ld nmounts %ld\n",
               device, size, gfs->gfs_super->sb_mounts);
        lc_initSnapshots(gfs, fs);
        for (i = 0; i <= gfs->gfs_scount; i++) {
            fs = gfs->gfs_fs[i];
            if (fs) {
                err = lc_readInodes(gfs, fs);
                if (err != 0) {
                    printf("Reading inodes failed, err %d\n", err);
                    return EIO;
                }
            }
        }
        fs = lc_getGlobalFs(gfs);
        lc_setupSpecialInodes(gfs, fs);
    }
    lc_blockAllocatorInit(gfs);

    /* Write out the file system super block */
    gfs->gfs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_RDWR;
    err = lc_superWrite(gfs, fs);
    if (err != 0) {
        printf("Superblock write failed, err %d\n", err);
    } else {
        *gfsp = gfs;
    }
    return err;
}

/* Sync a dirty file system */
static void
lc_sync(struct gfs *gfs, struct fs *fs) {
    int err;

    if (fs && (fs->fs_super->sb_flags & LC_SUPER_DIRTY)) {
        lc_lock(fs, true);
        lc_syncInodes(gfs, fs);
        lc_flushDirtyPages(gfs, fs);

        /* Flush everything to disk before marking file system clean */
        fsync(gfs->gfs_fd);
        fs->fs_super->sb_flags &= ~LC_SUPER_DIRTY;
        err = lc_superWrite(gfs, fs);
        if (err) {
            printf("Superblock update error %d for fs index %d root %ld\n",
                   err, fs->fs_gindex, fs->fs_root);
        }
        lc_unlock(fs);
    }
}

/* Free the global file system as part of unmount */
void
lc_unmount(struct gfs *gfs) {
    struct fs *fs;
    int i;

    lc_printf("lc_unmount: gfs_scount %d gfs_pcount %ld\n",
               gfs->gfs_scount, gfs->gfs_pcount);
    pthread_mutex_lock(&gfs->gfs_lock);

    /* Flush dirty data before destroying file systems since layers may be out
     * of order in the file system table and parent layers should not be
     * destroyed before child layers.
     */
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && !fs->fs_removed) {
            pthread_mutex_unlock(&gfs->gfs_lock);
            lc_sync(gfs, fs);
            pthread_mutex_lock(&gfs->gfs_lock);
        }
    }
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && !fs->fs_removed) {
            pthread_mutex_unlock(&gfs->gfs_lock);
            lc_freeLayerBlocks(gfs, fs, false);
            lc_destroyFs(fs, false);
            pthread_mutex_lock(&gfs->gfs_lock);
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    fs = lc_getGlobalFs(gfs);

    /* Combine sync and destroy */
    lc_sync(gfs, fs);
    lc_freeLayerBlocks(gfs, fs, false);
    lc_destroyFs(fs, false);
    lc_updateBlockMap(gfs);
    lc_blockAllocatorDeinit(gfs);
    lc_superWrite(gfs, fs);
    assert(gfs->gfs_count == 0);
    assert(gfs->gfs_pcount == 0);
    if (gfs->gfs_fd) {
        fsync(gfs->gfs_fd);
        close(gfs->gfs_fd);
    }
    lc_displayGlobalStats(gfs);
    free(fs->fs_super);
    free(fs);
    free(gfs->gfs_fs);
    free(gfs->gfs_roots);
    pthread_mutex_destroy(&gfs->gfs_lock);
    pthread_mutex_destroy(&gfs->gfs_alock);
}

/* Write out superblocks of all file systems */
void
lc_umountAll(struct gfs *gfs) {
    int i;

    for (i = 1; i <= gfs->gfs_scount; i++) {
        lc_sync(gfs, gfs->gfs_fs[i]);
    }
}
