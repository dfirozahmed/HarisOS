/* HarisOS volumes: puts the file systems found on the disks into the virtual file system (vfs.h), so that
 * the File Explorer shows them as drives under "This PC".
 *
 * volume_mount_all() looks at every partition (and every disk without a partition table) registered with
 * blk.h and mounts what it recognises: FAT32, exFAT and NTFS (all read and write). Other partitions
 * (ext4, BitLocker ...) are left alone. A drive starts LOCKED (read-only): see vfs_set_locked(). */
#ifndef HARIS_VOLUME_H
#define HARIS_VOLUME_H

/* Call after storage_init() and vfs_init(). Returns the number of drives mounted. */
int volume_mount_all(void);

/* How many partitions / disks were looked at but are not a file system we can read (for the Explorer's hint). */
int volume_unsupported_count(void);

#endif
