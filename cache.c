/*
    Caching file system proxy
    Copyright (C) 2004  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <pthread.h>

struct cache {
    int on;
    unsigned stat_timeout;
    unsigned dir_timeout;
    unsigned link_timeout;
    struct fuse_cache_operations *next_oper;
    GHashTable *table;
    pthread_mutex_t lock;
    time_t last_cleaned;
};

struct cache cache;

struct node {
    struct stat stat;
    int not_found;
    time_t stat_valid;
    char **dir;
    time_t dir_valid;
    char *link;
    time_t link_valid;
    time_t valid;
};

struct fuse_cache_dirhandle {
    const char *path;
    fuse_dirh_t h;
    fuse_dirfil_t filler;
    GPtrArray *dir;
};

static void free_node(gpointer node_)
{
    struct node *node = (struct node *) node_;
    g_strfreev(node->dir);
    g_free(node);
}

static int cache_clean_entry(void *key_, struct node *node, const time_t *now)
{
    (void) key_;
    if (*now > node->valid)
        return TRUE;
    else
        return FALSE;
}

static void cache_clean(void)
{
    time_t now = time(NULL);
    if (now > cache.last_cleaned + MIN_CACHE_CLEAN_INTERVAL &&
         (g_hash_table_size(cache.table) > MAX_CACHE_SIZE ||
          now > cache.last_cleaned + CACHE_CLEAN_INTERVAL)) {
        g_hash_table_foreach_remove(cache.table,
                                    (GHRFunc) cache_clean_entry, &now);
        cache.last_cleaned = now;
    }
}

static struct node *cache_lookup(const char *path)
{
    return (struct node *) g_hash_table_lookup(cache.table, path);
}

static void cache_purge(const char *path)
{
    g_hash_table_remove(cache.table, path);
}

static void cache_purge_parent(const char *path)
{
    const char *s = strrchr(path, '/');
    if (s) {
        if (s == path)
            g_hash_table_remove(cache.table, "/");
        else {
            char *parent = g_strndup(path, s - path);
            cache_purge(parent);
            g_free(parent);
        }
    }
}

static void cache_invalidate(const char *path)
{
    pthread_mutex_lock(&cache.lock);
    cache_purge(path);
    pthread_mutex_unlock(&cache.lock);
}

static void cache_invalidate_dir(const char *path)
{
    pthread_mutex_lock(&cache.lock);
    cache_purge(path);
    cache_purge_parent(path);
    pthread_mutex_unlock(&cache.lock);
}

static void cache_do_rename(const char *from, const char *to)
{
    pthread_mutex_lock(&cache.lock);
    cache_purge(from);
    cache_purge(to);
    cache_purge_parent(from);
    cache_purge_parent(to);
    pthread_mutex_unlock(&cache.lock);
}

static struct node *cache_get(const char *path)
{
    struct node *node = cache_lookup(path);
    if (node == NULL) {
        char *pathcopy = g_strdup(path);
        node = g_new0(struct node, 1);
        g_hash_table_insert(cache.table, pathcopy, node);
    }
    return node;
}

void cache_add_attr(const char *path, const struct stat *stbuf)
{
    struct node *node;

    pthread_mutex_lock(&cache.lock);
    node = cache_get(path);
    if (stbuf) {
      node->stat = *stbuf;
      node->not_found = 0;
    } else {
      node->not_found = 1;
    }
    node->stat_valid = time(NULL) + cache.stat_timeout;
    if (node->stat_valid > node->valid)
        node->valid = node->stat_valid;
    cache_clean();
    pthread_mutex_unlock(&cache.lock);
}

void cache_add_dir(const char *path, char **dir)
{
    struct node *node;

    pthread_mutex_lock(&cache.lock);
    node = cache_get(path);
    g_strfreev(node->dir);
    node->dir = dir;
    node->not_found = 0;
    node->dir_valid = time(NULL) + cache.dir_timeout;
    if (node->dir_valid > node->valid)
        node->valid = node->dir_valid;
    cache_clean();
    pthread_mutex_unlock(&cache.lock);
}

static size_t my_strnlen(const char *s, size_t maxsize)
{
    const char *p;
    for (p = s; maxsize && *p; maxsize--, p++);
    return p - s;
}

void cache_add_link(const char *path, const char *link, size_t size)
{
    struct node *node;

    pthread_mutex_lock(&cache.lock);
    node = cache_get(path);
    g_free(node->link);
    node->link = g_strndup(link, my_strnlen(link, size-1));
    node->not_found = 0;
    node->link_valid = time(NULL) + cache.link_timeout;
    if (node->link_valid > node->valid)
        node->valid = node->link_valid;
    cache_clean();
    pthread_mutex_unlock(&cache.lock);
}

static int cache_get_attr(const char *path, struct stat *stbuf)
{
    struct node *node;
    int err = -EAGAIN;
    pthread_mutex_lock(&cache.lock);
    node = cache_lookup(path);
    if (node != NULL) {
        time_t now = time(NULL);
        if (node->stat_valid - now >= 0) {
            if (node->not_found) {
              err = -ENOENT;
            } else {
              *stbuf = node->stat;
              err = 0;
            }
        }
    }
    pthread_mutex_unlock(&cache.lock);
    return err;
}

static int cache_getattr(const char *path, struct stat *stbuf)
{
    int err = cache_get_attr(path, stbuf);
    if (err == -EAGAIN) {
        err = cache.next_oper->oper.getattr(path, stbuf);
        if (!err)
            cache_add_attr(path, stbuf);
        else if (err == -ENOENT)
            cache_add_attr(path, NULL);
    }
    return err;
}

static int cache_readlink(const char *path, char *buf, size_t size)
{
    struct node *node;
    int err;

    pthread_mutex_lock(&cache.lock);
    node = cache_lookup(path);
    if (node != NULL) {
        time_t now = time(NULL);
        if (node->link_valid - now >= 0) {
            strncpy(buf, node->link, size-1);
            buf[size-1] = '\0';
            pthread_mutex_unlock(&cache.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&cache.lock);
    err = cache.next_oper->oper.readlink(path, buf, size);
    if (!err)
        cache_add_link(path, buf, size);

    return err;
}

static int cache_dirfill(fuse_cache_dirh_t ch, const char *name,
                         const struct stat *stbuf)
{
    int err = ch->filler(ch->h, name, 0, 0);
    if (!err) {
        char *fullpath;
        g_ptr_array_add(ch->dir, g_strdup(name));
        fullpath = g_strdup_printf("%s/%s", !ch->path[1] ? "" : ch->path, name);
        cache_add_attr(fullpath, stbuf);
        g_free(fullpath);
    }
    return err;
}

static int cache_getdir(const char *path, fuse_dirh_t h, fuse_dirfil_t filler)
{
    struct fuse_cache_dirhandle ch;
    int err;
    char **dir;
    struct node *node;

    pthread_mutex_lock(&cache.lock);
    node = cache_lookup(path);
    if (node != NULL && node->dir != NULL) {
        time_t now = time(NULL);
        if (node->dir_valid - now >= 0) {
            for(dir = node->dir; *dir != NULL; dir++)
                filler(h, *dir, 0, 0);
            pthread_mutex_unlock(&cache.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&cache.lock);

    ch.path = path;
    ch.h = h;
    ch.filler = filler;
    ch.dir = g_ptr_array_new();
    err = cache.next_oper->cache_getdir(path, &ch, cache_dirfill);
    g_ptr_array_add(ch.dir, NULL);
    dir = (char **) ch.dir->pdata;
    if (!err)
        cache_add_dir(path, dir);
    else
        g_strfreev(dir);
    g_ptr_array_free(ch.dir, FALSE);
    return err;
}

static int cache_unity_dirfill(fuse_cache_dirh_t ch, const char *name,
                               const struct stat *stbuf)
{
    (void) stbuf;
    return ch->filler(ch->h, name, 0, 0);
}

static int cache_unity_getdir(const char *path, fuse_dirh_t h,
                              fuse_dirfil_t filler)
{
    struct fuse_cache_dirhandle ch;
    ch.h = h;
    ch.filler = filler;
    return cache.next_oper->cache_getdir(path, &ch, cache_unity_dirfill);
}

static int cache_mknod(const char *path, mode_t mode, dev_t rdev)
{
    int err = cache.next_oper->oper.mknod(path, mode, rdev);
    if (!err)
        cache_invalidate_dir(path);
    return err;
}

static int cache_mkdir(const char *path, mode_t mode)
{
    int err = cache.next_oper->oper.mkdir(path, mode);
    if (!err)
        cache_invalidate_dir(path);
    return err;
}

static int cache_unlink(const char *path)
{
    int err = cache.next_oper->oper.unlink(path);
    if (!err)
        cache_invalidate_dir(path);
    return err;
}

static int cache_rmdir(const char *path)
{
    int err = cache.next_oper->oper.rmdir(path);
    if (!err)
        cache_invalidate_dir(path);
    return err;
}

static int cache_symlink(const char *from, const char *to)
{
    int err = cache.next_oper->oper.symlink(from, to);
    if (!err)
        cache_invalidate_dir(to);
    return err;
}

static int cache_rename(const char *from, const char *to)
{
    int err = cache.next_oper->oper.rename(from, to);
    if (!err)
        cache_do_rename(from, to);
    return err;
}

static int cache_link(const char *from, const char *to)
{
    int err = cache.next_oper->oper.link(from, to);
    if (!err) {
        cache_invalidate(from);
        cache_invalidate_dir(to);
    }
    return err;
}

static int cache_chmod(const char *path, mode_t mode)
{
    int err = cache.next_oper->oper.chmod(path, mode);
    if (!err)
        cache_invalidate(path);
    return err;
}

static int cache_chown(const char *path, uid_t uid, gid_t gid)
{
    int err = cache.next_oper->oper.chown(path, uid, gid);
    if (!err)
        cache_invalidate(path);
    return err;
}

static int cache_truncate(const char *path, off_t size)
{
    int err = cache.next_oper->oper.truncate(path, size);
    if (!err)
        cache_invalidate(path);
    return err;
}

static int cache_utime(const char *path, struct utimbuf *buf)
{
    int err = cache.next_oper->oper.utime(path, buf);
    if (!err)
        cache_invalidate(path);
    return err;
}

static int cache_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    int res = cache.next_oper->oper.write(path, buf, size, offset, fi);
    if (res >= 0)
        cache_invalidate(path);
    return res;
}

#if FUSE_VERSION >= 25
static int cache_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi)
{
    int err = cache.next_oper->oper.create(path, mode, fi);
    if (!err)
        cache_invalidate_dir(path);
    return err;
}

static int cache_ftruncate(const char *path, off_t size,
                           struct fuse_file_info *fi)
{
    int err = cache.next_oper->oper.ftruncate(path, size, fi);
    if (!err)
        cache_invalidate(path);
    return err;
}

static int cache_fgetattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
    int err = cache_get_attr(path, stbuf);
    if (err == -EAGAIN) {
        err = cache.next_oper->oper.fgetattr(path, stbuf, fi);
        if (!err)
            cache_add_attr(path, stbuf);
        else if (err == -ENOENT)
            cache_add_attr(path, NULL);
    }
    return err;
}
#endif

static void cache_unity_fill(struct fuse_cache_operations *oper,
                             struct fuse_operations *cache_oper)
{
#if FUSE_VERSION >= 23
    cache_oper->init        = oper->oper.init;
#endif
    cache_oper->getattr     = oper->oper.getattr;
    cache_oper->readlink    = oper->oper.readlink;
    cache_oper->getdir      = cache_unity_getdir;
    cache_oper->mknod       = oper->oper.mknod;
    cache_oper->mkdir       = oper->oper.mkdir;
    cache_oper->symlink     = oper->oper.symlink;
    cache_oper->unlink      = oper->oper.unlink;
    cache_oper->rmdir       = oper->oper.rmdir;
    cache_oper->rename      = oper->oper.rename;
    cache_oper->link        = oper->oper.link;
    cache_oper->chmod       = oper->oper.chmod;
    cache_oper->chown       = oper->oper.chown;
    cache_oper->truncate    = oper->oper.truncate;
    cache_oper->utime       = oper->oper.utime;
    cache_oper->open        = oper->oper.open;
    cache_oper->read        = oper->oper.read;
    cache_oper->write       = oper->oper.write;
    cache_oper->flush       = oper->oper.flush;
    cache_oper->release     = oper->oper.release;
    cache_oper->fsync       = oper->oper.fsync;
    cache_oper->statfs      = oper->oper.statfs;
    cache_oper->setxattr    = oper->oper.setxattr;
    cache_oper->getxattr    = oper->oper.getxattr;
    cache_oper->listxattr   = oper->oper.listxattr;
    cache_oper->removexattr = oper->oper.removexattr;
#if FUSE_VERSION >= 25
    cache_oper->create      = oper->oper.create;
    cache_oper->ftruncate   = oper->oper.ftruncate;
    cache_oper->fgetattr    = oper->oper.fgetattr;
#endif
}

struct fuse_operations *cache_init(struct fuse_cache_operations *oper)
{
    static struct fuse_operations cache_oper;
    cache.next_oper = oper;

    cache_unity_fill(oper, &cache_oper);
    if (cache.on) {
        cache_oper.getattr  = oper->oper.getattr ? cache_getattr : NULL;
        cache_oper.readlink = oper->oper.readlink ? cache_readlink : NULL;
        cache_oper.getdir   = oper->cache_getdir ? cache_getdir : NULL;
        cache_oper.mknod    = oper->oper.mknod ? cache_mknod : NULL;
        cache_oper.mkdir    = oper->oper.mkdir ? cache_mkdir : NULL;
        cache_oper.symlink  = oper->oper.symlink ? cache_symlink : NULL;
        cache_oper.unlink   = oper->oper.unlink ? cache_unlink : NULL;
        cache_oper.rmdir    = oper->oper.rmdir ? cache_rmdir : NULL;
        cache_oper.rename   = oper->oper.rename ? cache_rename : NULL;
        cache_oper.link     = oper->oper.link ? cache_link : NULL;
        cache_oper.chmod    = oper->oper.chmod ? cache_chmod : NULL;
        cache_oper.chown    = oper->oper.chown ? cache_chown : NULL;
        cache_oper.truncate = oper->oper.truncate ? cache_truncate : NULL;
        cache_oper.utime    = oper->oper.utime ? cache_utime : NULL;
        cache_oper.write    = oper->oper.write ? cache_write : NULL;
#if FUSE_VERSION >= 25
        cache_oper.create   = oper->oper.create ? cache_create : NULL;
        cache_oper.ftruncate = oper->oper.ftruncate ? cache_ftruncate : NULL;
        cache_oper.fgetattr = oper->oper.fgetattr ? cache_fgetattr : NULL;
#endif
        pthread_mutex_init(&cache.lock, NULL);
        cache.table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            free_node);
        if (cache.table == NULL) {
            fprintf(stderr, "failed to create cache\n");
            return NULL;
        }
    }
    return &cache_oper;
}

static const struct fuse_opt cache_opts[] = {
    { "cache=yes", offsetof(struct cache, on), 1 },
    { "cache=no", offsetof(struct cache, on), 0 },
    { "cache_timeout=%u", offsetof(struct cache, stat_timeout), 0 },
    { "cache_timeout=%u", offsetof(struct cache, dir_timeout), 0 },
    { "cache_timeout=%u", offsetof(struct cache, link_timeout), 0 },
    { "cache_stat_timeout=%u", offsetof(struct cache, stat_timeout), 0 },
    { "cache_dir_timeout=%u", offsetof(struct cache, dir_timeout), 0 },
    { "cache_link_timeout=%u", offsetof(struct cache, link_timeout), 0 },
    FUSE_OPT_END
};

int cache_parse_options(struct fuse_args *args)
{
    cache.stat_timeout = DEFAULT_CACHE_TIMEOUT;
    cache.dir_timeout = DEFAULT_CACHE_TIMEOUT;
    cache.link_timeout = DEFAULT_CACHE_TIMEOUT;
    cache.on = 1;

    return fuse_opt_parse(args, &cache, cache_opts, NULL);
}
