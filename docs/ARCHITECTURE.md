# Architecture

## The two-layer design

DiskForge splits cleanly into an **engine** and an **app**:

- **`src/engine`** — plain C++17, zero UI dependencies, compiles on Linux and Windows. Everything that understands *bytes on a disk* lives here. This is what makes the test suite real: the engine runs identically in CI on Ubuntu against synthetic disk images and on Windows against VHDs.
- **`src/app`** — the WinUI 3 front end. It never touches `\\.\PhysicalDrive` handles or sector buffers directly; it calls `df::ops::*` functions and renders results.

## Engine modules

### Partition tables
`mbr.*` parses/writes MBR including **EBR chains** (logical partitions inside extended partitions — walk, append, rewrite). `gpt.*` reads/writes both header copies + entry arrays with CRC validation, and can rebuild a damaged primary from the backup (and vice versa).

`layout.*` is the unified model both layers speak: one `PartInfo` per allocated range regardless of table style, with free-extent computation, 1 MiB alignment, and slot finding. `ReadFromTables` reconstructs a layout from raw sectors — used by tests, the lost-partition flow, and as the ground truth under the ioctl view on Windows.

### File systems

**FAT (`fat.*`)** — the most complete module:
- FAT12/16/32 parsing; enumeration with LFN reconstruction (spec-ordered entries, strict checksum for alive files, lenient for deleted ones where the checksum is definitionally broken).
- Deleted-file recovery via the contiguous-cluster heuristic (chain zeroed on delete).
- **Offline resize** — the interesting part. The FAT size class can change, which moves the entire data region. The algorithm:
  1. If the new cluster count is smaller, **compact** clusters below the limit (monotone remap → forward sweep is always safe).
  2. Snapshot FAT0, move the data region (overlap-aware direction), write new FATs from the snapshot, update BPB/FSInfo/backup boot.
  Compaction rewrites directory entries' start-cluster fields (including `.`/`..`) via a recorded patch table.

**NTFS (`ntfs.*`)** — read-oriented: boot BPB, `$MFT` through fragmented runlists (with a one-cluster cache for sequential scans), update-sequence-array fixups, `$FILE_NAME`/`$DATA` attribute parsing (resident + non-resident), `$Bitmap` usage counting, deleted-record and orphan detection, and data extraction honoring sparse runs.

### Recovery
- `carve.*` — signature scanner with per-type strategies: header size fields (BMP/7z/WAV), footer scans (JPEG/PNG/PDF/ZIP EOCD), exact MP4 atom walking (big-endian sizes), and next-signature bounding for unknown-length types. Strict plausibility checks (DIB header, WAVE magic, planes/bpp...) keep false positives near zero in random data.
- `scan.*` — lost-partition scanner: GPT health report (primary/backup damage), quick scan on the 1 MiB grid, full sector-by-sector scan, candidate validation through `ProbeFs` (which cross-checks BPB arithmetic, FAT[0] patterns, `$MFT` reachability, backup boot sectors), overlap flags and confidence scoring.

### Windows service layer (`ops_win.*`)
All OS interaction: `IOCTL_DISK_GET/SET_DRIVE_LAYOUT_EX` (MBR logicals handled via engine EBR writes), `IOCTL_DISK_CREATE_DISK`, `FSCTL_SHRINK_VOLUME` (prepare/commit) for NTFS shrink, `FSCTL_EXTEND_VOLUME` for NTFS/exFAT extend, volume/letter management via mount points, dismount/lock, formatting (Windows format engine with the DiskForge FAT32 formatter as fallback), and the composite operations:

- **Move** — copy (overlap-safe) → patch boot sector hidden-sectors (+FAT32 backup boot / NTFS backup boot) → update table → restore letter. Table changes come last, so cancellation never corrupts.
- **Merge** — file-level copy into a `Merged_...` folder first; only after a verified copy is the source partition removed and the target extended.
- **Allocate free space / 1-click extend** — a "slide" loop that moves a free extent next to the target by shifting the partitions in its path (each move is the verified primitive above), then extends.

## App structure
MVVM-ish: pages + dialogs are XAML runtime classes; `ViewModels.idl` defines the bindable runtime classes (get-only properties, `Visibility` helpers); `AppContext` is the singleton bridge — background job threads, progress marshaled onto the UI `DispatcherQueue`, a lock-guarded log ring + file sink, and settings. The partition map is built from code-behind (proportional `Border` elements) so it stays DPI- and resize-correct without custom controls.
