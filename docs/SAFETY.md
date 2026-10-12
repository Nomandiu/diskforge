# Safety design

DiskForge performs irreversible operations. These are the mechanisms that keep them safe:

## Ordering rules
- **Copy first, table last.** Moves and space-sliding copy the data to the new location and only then update the partition entry. A crash, power loss, or cancel during the copy leaves the original partition entry pointing at the original (intact) data.
- **Dismount before offline FS work.** Engine-level FAT32 resizes run after a volume lock + dismount; online NTFS operations use the OS's own FSCTL path.
- **Merge never destroys on failure.** The source partition is deleted only after the file copy completed and was verified to begin; any copy error aborts the merge with both partitions intact.

## Validation layers
1. UI dialogs clamp sizes to free extents and rough minimums.
2. Engine operations re-validate against the *live* layout (overlap, alignment, disk bounds) before writing anything.
3. FS-level minimums are enforced (FAT32 `MinSectors`; NTFS ShrinkPrepare for the true OS-reported minimum).

## Confirmation policy
- Deletion, formatting, merge, and disk initialization always show explicit danger confirmations.
- A global **read-only mode** (Settings) blocks every mutating call at the `RunOp` gate.
- Non-destructive operations (scan, recover-to-folder, labels) confirm only when they relocate data.

## What is deliberately refused
- Moving the partition that hosts the running Windows installation (online move of C: is not safe; use extend flows instead).
- Shrinking below the file-system minimum.
- Restoring a lost partition that would overlap an existing one (reported, not silently merged).

## Recovery-net behaviors
- All table writes go through `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` so Windows maintains both GPT copies; engine-level repair writes backup structures first.
- Deleted partitions remain recoverable via Partition recovery until their sectors are reused.
- Recovered files are always written to a *different* location (with a same-disk warning) to avoid overwriting the very data being recovered.

## Logging
Every operation, error, and cancel is logged with timestamps to `%APPDATA%\DiskForge\diskforge.log` (2 MiB rotating) and viewable/exportable in the Log page — the record needed to reconstruct what happened on a disk.
