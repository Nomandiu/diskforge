# Testing

## Unit tests (51, run on Linux + Windows CI)

| Suite | Covers |
|---|---|
| test_core | CRC32 known vectors, GUID parse/format/mixed-endian, UTF conversion round-trips, LE helpers, size formatting/parsing |
| test_tables | MBR round-trip + protective MBR + hide/unhide types, EBR chain walk/append, GPT write/read round-trip + backup-recover, layout parse (GPT + MBR-with-logical), free extents, FindSlot, WriteToTables round-trip, FS probe, overlap-safe sector copy (forward + backward) |
| test_fat | format+open, add/read files, LFN write/read, delete + deleted-content recovery, deleted LFN recovery, shrink keeps files byte-exact, extend across FAT size class (region move) + post-move writes, shrink-too-far refusal, cluster compaction with map verification, label, shrink→extend integrity, min-size growth |
| test_ntfs | open + label, alive/deleted enumeration, resident + deleted-resident extraction, non-resident multi-run extraction (fragmented, exact bytes), $Bitmap usage count, detection, runlist decoder unit cases (sparse, malformed, multi-byte, sign-extended) |
| test_carve | JPEG/PNG/PDF/ZIP with exact sizes, MP4 exact atom-walk sizing, type filters, embedded-file non-overlap |
| test_scan | deleted GPT partitions found (exact start/size/fs), restored partitions keep files, overlap flagging, unaligned full-mode scan, GPT repair from backup, MBR deleted-entry recovery |

## Integration suite (Windows CI, real VHDs)

`df-tests --integration \\.\PhysicalDriveN` runs against a 512 MiB fixed VHD created and attached by diskpart in CI:

1. Enumerate + initialize GPT
2. Create 48 MiB FAT32 partition, write files through Win32
3. **Shrink** by 8 MiB → engine-verify file content unchanged
4. **Extend** back → verify again
5. Create NTFS partition, write file, **move** it 4 MiB → verify
6. **Label** + **drive letter** change/restore
7. **Hide/unhide** (GPT attribute verified on disk)
8. **Split**
9. **Delete** + **scan** + **restore** the lost partition
10. Delete a file via Win32 → **RecoverySession** recovers it with exact content
11. Minimum-size query

## App smoke test (Windows CI)

The built `DiskForge.exe` is launched and must keep its window alive for 8 seconds.

## How to run locally

```bash
# engine tests (Linux/macOS/Windows)
cmake -S . -B build && cmake --build build && (cd build && ctest)

# integration (Windows, elevated, uses the given disk - DATA WILL BE ERASED)
build/Release/df-tests.exe --integration \\.\PhysicalDrive2
```
