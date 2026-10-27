/* Shared by the files of the virtual file system (kernel/fs/vfs/): the volume record and the helpers. The public interface is include/vfs.h. */
#ifndef HARIS_VFS_INTERNAL_H
#define HARIS_VFS_INTERNAL_H

#include "vfs.h"
typedef struct
{
    const VfsBackend *ops;
    void *ctx;
    int root; /* the volume's folder node */
} Volume;

/* ---- vfs_core.c ---- */
extern VfsNode NODES[VFS_MAX_NODES];
extern Volume VOLS[VFS_MAX_VOLUMES];
extern int NVOLS;
extern unsigned GEN;
extern int LAST_ERR;
int fail(int code);
int lower(int c);
int same_exact(const char *a, const char *b);
void copy_name(char *dst, const char *src);
void copy_fs(unsigned char *dst, const unsigned char *src);
int free_slot(void);
int new_node(int parent, const char *name, int is_dir, unsigned size, const unsigned char *fs);
void drop_subtree(int id);
const VfsBackend *ops_of(int id);
void *ctx_of(int id);
void batch(int id, int on);
int find_child(int dir, const char *name);
int name_len(const char *name);

/* ---- vfs_mount.c ---- */
void ensure_loaded(int dir);
void unique_name(int dir, const char *name, int is_dir, char *out);

/* ---- vfs_edit.c ---- */
void unique_name(int dir, const char *name, int is_dir, char *out);

#endif
