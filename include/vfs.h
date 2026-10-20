/* HarisOS virtual file system: a small in-memory tree of folders and files (no malloc).
 *
 * This is a RAM volume: nothing is read from or written to a disk yet. It gives the File Explorer
 * a real tree to browse; a disk driver (FAT32) can later fill the same tree or replace the backend.
 * Nodes are identified by an id; the root folder is VFS_ROOT. */
#ifndef HARIS_VFS_H
#define HARIS_VFS_H

#define VFS_MAX_NODES 96
#define VFS_NAME_MAX 32 /* including the final 0 */
#define VFS_ROOT 0

typedef struct
{
    char name[VFS_NAME_MAX]; /* "" for the root */
    int used;
    int is_dir;
    unsigned size; /* bytes; always 0 for a folder */
    int parent;    /* -1 for the root */
} VfsNode;

/* Empty tree (just the root), then the demo content. vfs_reset() alone gives only the root. */
void vfs_reset(void);
void vfs_init(void);

/* Add a file or folder under `parent`. Returns the new id, or -1 if parent is not a folder, the name is
 * empty / too long / contains '/', the name already exists there, or the pool is full. */
int vfs_add(int parent, const char *name, int is_dir, unsigned size);

const VfsNode *vfs_node(int id); /* NULL if id is not a used node */
int vfs_count(void);             /* used nodes, including the root */

/* The children of `dir` into out[] (at most max), folders first, then by name (ignoring case).
 * Returns how many there are in total (may be more than max). */
int vfs_list(int dir, int *out, int max);

/* "/Documents/readme.txt" for a node ("/" for the root). Always 0-terminated, cut if cap is too small.
 * Returns the length written. */
int vfs_path(int id, char *out, int cap);

/* Find a node by absolute path ("/", "/Documents", "/Documents/readme.txt"). -1 if not found. */
int vfs_lookup(const char *path);

/* Rename a node. -1 if the name is empty / too long / has '/', a sibling already has it (ignoring case),
 * or id is the root. Giving a node the same name with other capitals is allowed. */
int vfs_rename(int id, const char *name);

/* Delete a node and everything below it. Returns how many nodes were removed, -1 for the root or a bad id.
 * Ids of removed nodes may be reused by later vfs_add() calls. */
int vfs_remove(int id);

/* How many nodes lie below `id` (0 for a file or an empty folder). */
int vfs_subtree_count(int id);

/* 1 if `id` is `ancestor` or lies below it. */
int vfs_is_under(int id, int ancestor);

#endif
