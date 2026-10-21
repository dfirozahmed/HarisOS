/* HarisOS volumes: puts the file systems found on the disks into the virtual file system (vfs.h), so that
 * the File Explorer shows them as drives under "This PC".
 *
 * volume_mount_all() looks at every partition (and every disk without a partition table) registered with
 * blk.h and mounts what it recognises: FAT32, exFAT and NTFS (all read and write). Other partitions
 * (ext4, BitLocker ...) are left alone. A drive starts LOCKED (read-only): see vfs_set_locked(); the clean ones are
 * unlocked right after by volume_unlock_clean(), and those have no lock any more (VfsVolInfo.lockable = 0). */
#ifndef HARIS_VOLUME_H
#define HARIS_VOLUME_H

/* Call after storage_init() and vfs_init(). Returns the number of drives mounted. */
int volume_mount_all(void);

/* Unlock the disks whose mounted drives are all clean (not dirty, not hibernated), so they can be written without
 * asking and stops offering the lock for them. A disk with any drive that is not clean stays locked, with its lock.
 * Call after volume_mount_all(). Returns how many
 * disks were unlocked. */
int volume_unlock_clean(void);

/* How many partitions / disks were looked at but are not a file system we can read (for the Explorer's hint). */
int volume_unsupported_count(void);

#endif
