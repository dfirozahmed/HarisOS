/* HarisOS volumes: puts the file systems found on the disks into the virtual file system (vfs.h), so that
 * the File Explorer shows them as drives under "This PC".
 *
 * volume_mount_all() looks at every partition (and every disk without a partition table) registered with
 * blk.h and mounts what it recognises: FAT32, exFAT and NTFS (all read and write). Other partitions
 * (ext4, BitLocker ...) are left alone. A drive starts LOCKED (read-only): see vfs_set_locked(). volume_unlock_clean()
 * then unlocks the ordinary clean ones for good (VfsVolInfo.lockable = 0: no lock button); the EFI System Partition,
 * the Windows drive and any drive that is not clean stay locked, with a lock the user can open. The lock is per
 * partition. */
#ifndef HARIS_VOLUME_H
#define HARIS_VOLUME_H

/* Call after storage_init() and vfs_init(). Returns the number of drives mounted. */
int volume_mount_all(void);

/* Unlock the drives that are clean (not dirty, not hibernated) and are not system drives (the EFI System Partition,
 * the NTFS volume Windows is installed on), so they can be written without asking; they stop offering the lock.
 * Everything else stays locked and keeps its lock. Call after volume_mount_all(). Returns how many drives were
 * unlocked. */
int volume_unlock_clean(void);

/* How many partitions / disks were looked at but are not a file system we can read (for the Explorer's hint). */
int volume_unsupported_count(void);

#endif
