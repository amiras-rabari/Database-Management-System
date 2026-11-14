# DESIGN_M1 — storage engine and buffer pool

Required contents (SPEC §M1.11): catalog page format, free space strategy, compaction decision, a byte-level diagram of a real page from our own files (page header, slot directory, record format), plus reasoning. The diagrams in §2–§4 are real bytes, dumped with `od` from a database made by the commands below; `storage layout` prints the same numbers.

```
storage create-table --db db --name R --schema "a:INT,b:VARCHAR(20),c:INT"
storage load         --db db --table R --file r.csv      # 42,bob,7 / 1,abcdefghij,2 / 43,amy,9
storage layout       --db db --table R --page 1
```

Rules that hold everywhere: pages are 4096 bytes, page ids start at 0 in every file, all integers are little-endian, there is no file header outside pages, and a database folder holds `<table>.db` per table, one `catalog.db`, and a `tmp/` folder for temporary files.

## 1. Files and pages

| File | Contents |
|---|---|
| `R.db` | heap file of table R: directory pages + data pages (§3, §4) |
| `catalog.db` | slotted pages holding the catalog records (§6) |
| `tmp/temp_000001.db`, … | exec's sort runs and partitions; deleted when dropped (§9) |

File size is always `pages × 4096`. `DiskManager::allocatePage` appends a zero page at once, so a file never loses an allocated page on reopen; allocating is **not** counted as a write (D2), only real `writePage` calls are.

**At most 256 files are open at once (added 2026-11-08).** A file's stream is opened on its first page call. `FileRegistry` keeps at most `kMaxOpenFiles` = 256 streams open and closes the least recently used one when another file needs one; its next page call simply reopens it. Opening and closing a file is not a page read or write, so no counter changes: `REPORT_M1.md`'s numbers were re-measured afterwards and are identical.

- **What happened.** Before this, every `DiskManager` kept its file open for its whole life. That was fine for M1's few tables. But M2's Grace hash join uses P = B − 1 partition files for each input, so at B = 256 it keeps 2 × 255 = 510 temporary files open at once. On Windows a program can have only about 512 files open, so the join failed while creating file 507 ("Cannot create file …temp_000507.db"). Our unit tests never used that many files; the problem was found by the M2 acceptance run (`bench/exec_acceptance.py`), which runs every join at B = 4, 16, 64 and 256 (§M2.6 measurement 1).
- **Linux too, for safety.** On Linux the default limit is 1024 open files, so B = 256 would have worked there, but a larger `--frames` (about 512 or more) would fail the same way. B comes from `--frames` and has no upper limit, so the limit is in storage and applies on every system.
- **Why this design.** Opening the file for every page call and closing it again was tried first. It was correct, but much slower (exec's tests went from about 10 s to 71 s, storage's to 48 s), because opening a file costs far more than reading one page. Keeping the 256 most recently used files open is as fast as before, since a plan rarely uses more files than that at once. 256 is below both the Windows (about 512) and Linux (1024) limits.
- **Tested** by a storage case that writes and reads back one page in each of 600 files (at most 256 open at any time). Removing the close makes it fail exactly as the hash join did, at file 509.

## 2. Record format (§M1.3)

A record is its fields in schema order, nothing else: no header, no null bitmap (there are no NULLs). INT = 4 bytes two's complement; VARCHAR(n) = u16 byte length L (L ≤ n ≤ 255) + L bytes. The same rule is reused for every string in the catalog.

Real record of `(1, 'abcdefghij', 2)` from `R.db`, bytes 4063–4082 of page 1:

| Bytes | Field | Value |
|---|---|---|
| `01 00 00 00` | a INT | 1 |
| `0a 00` | b length | 10 |
| `61 62 63 64 65 66 67 68 69 6a` | b bytes | `abcdefghij` |
| `02 00 00 00` | c INT | 2 |
| | **length** | **20 bytes** |

## 3. Data page (slotted page, §M1.4): real page 1 of `R.db`

```
offset  0          8                20                                  4050      4063              4083     4095
        +----------+----------------+-------- free space (4030) --------+---------+-----------------+--------+
        | header   | slot 0..2      |                                   | rec 2   | rec 1           | rec 0  |
        +----------+----------------+-----------------------------------+---------+-----------------+--------+
          slots grow ->                                         <- records grow (newest at the lowest offset)
```

| Offset | Bytes (real) | Field | Value |
|---|---|---|---|
| 0 | `01 00 00 00` | page_id u32 | 1 |
| 4 | `03 00` | slot_count u16 (dead slots included) | 3 |
| 6 | `d2 0f` | free_end u16 | 4050 |
| 8 | `f3 0f 0d 00` | slot 0 (offset, length) | (4083, 13) |
| 12 | `df 0f 14 00` | slot 1 | (4063, 20) |
| 16 | `d2 0f 0d 00` | slot 2 | (4050, 13) |
| 20–4049 | `00 …` | free space = 4050 − (8 + 4·3) | 4030 bytes |
| 4050 | `2b 00 00 00 03 00 61 6d 79 09 00 00 00` | record of slot 2 | (43, 'amy', 9) |
| 4063 | `01 00 00 00 0a 00 61 … 6a 02 00 00 00` | record of slot 1 | (1, 'abcdefghij', 2) |
| 4083 | `2a 00 00 00 03 00 62 6f 62 07 00 00 00` | record of slot 0 | (42, 'bob', 7) |

`storage layout --db db --table R --page 1` prints exactly this page:

```
table R, page 1 of 2 (data page)
offset  size  field         value
     0     4  page_id       1
     4     2  slot_count    3
     6     2  free_end      4050
free space = free_end - (8 + 4 x slot_count) = 4030 bytes
slot  at  offset  length  state  record
   0   8    4083      13  live   bytes 4083..4095  (42, 'bob', 7)
   1  12    4063      20  live   bytes 4063..4082  (1, 'abcdefghij', 2)
   2  16    4050      13  live   bytes 4050..4062  (43, 'amy', 9)
```

These are rows 2–4 of the §M1.4 worked table (records of 13, 20 and 13 bytes). Row 5, "delete slot 1", gives slot 1 = `00 00 00 00` (dead), slot_count 3, free_end 4050, free space 4030: proven by the test `page_layout_tests.cpp` through `HeapFile::remove` and `describePage` (no CLI command deletes).

**Rules.** A slot with offset 0 is dead. Slot numbers are never reused or renumbered, so a rid `(page, slot)` stays valid forever. An insert of L bytes needs L + 4 bytes of free space (the record + a new slot). Capacity for fixed-length records: `floor(4088 / (L + 4))`, 240 for L = 13.

**Largest record (D5).** The spec says records up to 4088 bytes; an empty page has 4096 − 8 = 4088 free bytes, but a record also needs its 4-byte slot, so the largest record that really fits is **4084** bytes. We keep the spec's constant `kMaxRecordLength = 4088` and reject every record that cannot be stored (L > 4084) with `error: schema: Table 'R': record too long (N bytes, at most 4084 fit on a page): row (…)`, naming the table and the row; the loader adds the line number.

## 4. Directory page and page order (§M1.5): real page 0 of `R.db`

| Offset | Bytes (real) | Field | Value |
|---|---|---|---|
| 0 | `00 00 00 00` | page_id u32 | 0 |
| 4 | `ff ff ff ff` | next_dir_page u32 | none |
| 8 | `01 00` | entry_count u16 | 1 |
| 10 | `01 00 00 00 be 0f` | entry 0: data_page_id u32, free_bytes u16 | page 1, 4030 |
| 16– | `00 …` | room for 680 more entries | 681 per page = floor((4096 − 10) / 6) |

```
table R, page 0 of 2 (directory page)
offset  size  field         value
     0     4  page_id       0
     4     4  next_dir_page none (0xFFFFFFFF)
     8     2  entry_count   1
entry  at  data_page  free_bytes
    0  10          1        4030
```

**Page order.** Page 0 is the first directory page. A new directory page is always added *before* the first data page it lists, so directory pages are exactly the page ids that are multiples of **682** (1 directory + 681 data pages). Data page p is listed on directory page `p / 682 × 682`, at entry `p − dir − 1`; e.g. page 700 is entry 17 of directory page 682. `get`, `remove`, `numDataPages` and `layout` therefore find pages by arithmetic, with no directory walk. `insert` checks every new data page against this rule before listing it and reports a corrupt file if they disagree.

**Why a directory.** It lets an insert find a page with room by reading directory pages only (681 data pages per directory page read), and it gives the scan its order. §M1.5 allows a scan at most one directory page and one data page at any moment; ours pins **one page at a time** (changed 2026-11-01 so exec's seqscan fits its budget of 1 frame, §M2.3). Since 2026-11-07 it reads each directory page **once**: it pins it, notes its entry count and next directory page, and unpins it, then visits the data pages by number. That is safe because entry i of directory page d is always data page d + 1 + i (insert adds pages in that order and refuses anything else). So it works with B = 1, and every page of the table is fetched exactly once at any B and with either policy. The first version (2026-11-01) fetched the directory page again before every data page; that cost nothing while the scan had a spare frame, but under an exec operator that reserves every other frame (sort, hash join, nested loop) it became a second disk read per data page.

## 5. Free space strategy

- **First fit** (§M1.5): `insert` walks the directory pages in order and takes the first entry with `free_bytes ≥ L + 4`. If none has room, it appends a new data page (and first a new directory page when the last one is full).
- `free_bytes` in an entry is always the page's real free space after the change, `free_end − (8 + 4 × slot_count)`, so the directory never promises room the page does not have (an insert that finds otherwise reports a corrupt file).
- **Reasoning.** First fit is what the spec's pseudocode does, is deterministic (same inserts → same rids and the same files), and fills early pages before growing the file. Its cost is a directory walk per insert, which reads only directory pages (1 per 681 data pages), so it stays small even for the 500-page table of §M1.10.
- Pins: an insert holds at most the directory page and one data page (2 pins); `remove` releases the data page before fetching the directory page (1 pin at a time).

## 6. Compaction decision: none

We do **not** compact (D3). `remove` only sets the slot to `(0, 0)`; the record's bytes stay as an unreachable hole, `free_end` does not move, and the directory's `free_bytes` is rewritten with the same value (we still rewrite it, as the pseudocode says, so the code stays correct if compaction is ever added).

Reasons: compaction is optional in §M1.4; the §M1.4 worked example ("deleting a 20-byte record freed no usable space") and every counter in the report then match the spec exactly; slots must never be renumbered anyway, so compaction could only slide record bytes and fix offsets, which adds code and a write of the page for a space gain the workloads of this course (bulk load, then read) never need. Consequence, stated in the report where it matters: a table with many deletes keeps its pages; a scan still reads them all.

## 7. Buffer pool

One pool of B frames (allocated once) is shared by every file; a page is named by `(FileId, PageId)`. The pool is the only place page bytes live in memory, so "at most B pages in memory" holds by construction. A frame with pin count > 0 is never chosen as a victim; when every frame is pinned, `fetchPage` fails with `std::logic_error` "No unpinned frame available"; unpinning at pin count 0 is an error too (the required test). Free frames are used first, lowest index first; filling one is not an eviction.

**Policies.** LRU: a logical clock (a counter, +1 on every hit and load) stamps each frame; the victim is the unpinned frame with the smallest stamp; deterministic, no wall clock. CLOCK: the spec's sweep over reference bits (a hit sets ref = 1; the hand clears ref = 1 frames and takes the first unpinned ref = 0 frame); the hand moves past a frame filled from the free list. Default policy is LRU in every tool. §M1.6 trace `1 2 3 1 4 2`, B = 3: LRU 1 hit, 5 misses, 2 evictions; CLOCK 2 hits, 4 misses, 1 eviction (`storage trace` reproduces both).

### Counting rules (decided 2026-10-25, chunk 05)

`newPage` is not a fetch and reads nothing from disk, so by §M1.6 it is never a miss ("a fetch that required a disk read") and therefore never an eviction ("a miss that replaced a resident page"), even when it takes the frame of a resident page. If that page is dirty, writing it back is one call to the disk manager's write and counts one write.

| Event | hit | miss | eviction | read | write |
|---|---|---|---|---|---|
| `fetchPage`, page resident | +1 | | | | |
| `fetchPage` into a never-used frame | | +1 | | +1 | |
| `fetchPage` replacing a clean page | | +1 | +1 | +1 | |
| `fetchPage` replacing a dirty page | | +1 | +1 | +1 | +1 |
| `newPage` (free frame, or replacing a clean page) | | | | | |
| `newPage` replacing a dirty page | | | | | +1 |
| `flushPage` / `flushAll`, per dirty page | | | | | +1 |
| `discardFile` (dropping a temp file) | | | | | |

**Attribution.** Counters only grow; a command's counters are the difference of two snapshots. The catalog is read once when the engine opens (D4), so every command takes its first snapshot after opening and after the table lookup: catalog pages never appear in `scan`'s counters. Example from our files: `scan --frames 8` of R (1 directory + 1 data page) reports `{"hits":1,"misses":2,"evictions":0,"reads":2,"writes":0}` (the hit is the second fetch of the directory page, which finds no next entry), identical on every run.

## 8. Catalog page format (Version 2, DECISIONS 2026-10-27)

`catalog.db` is a row of ordinary slotted pages (§3 layout), one catalog record per slot. The first byte of each record is its **kind**. There is no header record: the number of catalog pages is the file's page count, and the number of tables is the number of kind-1 records. A record never spans pages (≤ 4084 bytes).

| Kind | Record | Count |
|---|---|---|
| 1 | table record | one per table |
| 2 | statistics chunk | one or more per analyzed table (M4) |

Strings ("name" below) are a u16 byte length + the bytes, the VARCHAR rule of §2.

**Table record (kind 1)**

| Field | Type |
|---|---|
| kind | u8 = 1 |
| table name | name |
| data file name | name (`R.db`) |
| first page | u32 (always 0) |
| num pages | u32, **data pages only** |
| num live tuples | u64 |
| column count | u16 |
| per column: name, type, n | name (bare, `a` not `R.a`), u8 (0 INT, 1 VARCHAR), u16 VARCHAR length (0 for INT) |

Real page 0 of `catalog.db` after the three-row load: page_id `00 00 00 00`, slot_count `01 00`, free_end `d2 0f` (4050), slot 0 `d2 0f 2e 00` = (4050, 46). The 46-byte record at 4050:

| Bytes (real) | Field | Value |
|---|---|---|
| `01` | kind | table record |
| `01 00 52` | table name | `R` |
| `04 00 52 2e 64 62` | data file | `R.db` |
| `00 00 00 00` | first page | 0 |
| `01 00 00 00` | num pages (data) | 1 |
| `03 00 00 00 00 00 00 00` | num live tuples | 3 |
| `03 00` | column count | 3 |
| `01 00 61` `00` `00 00` | column 1 | a, INT |
| `01 00 62` `01` `14 00` | column 2 | b, VARCHAR(20) |
| `01 00 63` `00` `00 00` | column 3 | c, INT |

The smallest example, `T(a INT)` with no pages or rows, is 34 bytes and is checked byte for byte by a test: `01 | 01 00 54 | 04 00 54 2E 64 62 | 00 00 00 00 | 00 00 00 00 | 00×8 | 01 00 | 01 00 61 00 00 00`.

Column names are stored bare because the table name is already in the record; `getTable` returns them qualified (`R.a`) as every later module expects. The data file name is derivable but stored because §M1.7 asks for it.

**Statistics (room for M4).** All of one table's statistics are encoded as **one blob**, then cut **by bytes** into kind-2 chunks:

| Kind-2 chunk | Type |
|---|---|
| kind | u8 = 2 |
| table name | name |
| chunk number, chunk count | u16, u16 (0 … count − 1) |
| payload | the rest of the record (≤ 4084 − header bytes) |

| Blob field | Type |
|---|---|
| nTuples, nPages, width | u64, u32, u64 = the 8 raw bytes of the `double` (`std::bit_cast`, exact round trip) |
| column count | u16 |
| per column: ndv, min, max | u64, value, value (value = the column's §M1.3 encoding: INT 4 bytes, VARCHAR u16 + bytes) |
| bucket count, then per bucket: lo, hi, count, ndv | u16; value, value, u64, u64 |

Reading joins chunks 0 … count − 1 in order (a missing, extra or out-of-order chunk is a corrupt file) and decodes once with the table's schema. Why chunks: `--buckets k` has no upper limit, so statistics can exceed one record for any column type; one INT bucket is 24 bytes, so `--buckets 400` makes one INT column 8 + 4 + 4 + 2 + 400 × 24 = 9 618 bytes, about three chunks; a VARCHAR(255) bucket with 255-byte bounds is 530 bytes. Cutting by bytes needs one generic cut/join helper that knows nothing about statistics; M4 adds kind-2 records without changing kind-1 records, and a table with no kind-2 record has "no statistics" (no NULLs needed).

**When the catalog is read and written (D4).** `StorageEngine::open` reads every catalog page once through the buffer pool into an in-memory map of table metadata (never tuples). Changes mark it changed; `flushAll`/`close` rewrite the pages from page 0 (table records by name, then statistics chunks), only if something changed. If fewer pages are needed than the file has, the leftover pages are rewritten empty (the file never shrinks). The same operations in two folders give byte-identical `catalog.db` files (tested).

**Names.** A table name is letters, digits and `_` only (it becomes a file name, so `/` and `..` are impossible), is not `catalog`, and must not already exist; these are `error: schema:` errors. Names are case-sensitive everywhere, so `Emp` and `emp` are different tables, as the sql spec requires. On a case-insensitive file system (Windows) `emp.db` and `Emp.db` are one file; creating the second table then fails safely with `error: name: … already exists`, because a file is never overwritten (a documented Windows limitation; Linux behaves as the spec says).

## 9. Temporary files

Only storage touches files, so exec gets temporary files from `StorageEngine::createTempFile()`: a move-only handle owning `tmp/temp_000001.db`, `temp_000002.db`, … (a counter per engine open; never time or random names). Its pages use the same B frames and are counted like any other. Dropping it (explicitly, at scope exit, or during an exception) discards its frames **without writing** and deletes the file, so a dropped run costs no writes nobody reads. Leftover `temp_<digits>.db` files from a crash are deleted when the engine opens; nothing else is touched.

## 10. Loader (CSV)

One line at a time (never the whole file), split by a hand-written state machine (no `<regex>`): fields separated by commas; a field starting with `"` is quoted, may contain commas, and `""` inside it is one `"`; a trailing `\r` is removed. Each field goes through common's `parseValue` (the project's single "valid INT" rule). A bad INT, a VARCHAR longer than n, a wrong number of fields (also a blank line), or a record too long stops the load with an error naming the line and the column, e.g. `error: schema: line 2, column 2 (b): 'toolongtoolongtoolongX' is longer than VARCHAR(20) …`. Rows before the bad line stay, and the catalog's page and tuple counts are updated to match them.

## 11. Readings cited

Both are from Andy Pavlo's CMU 15-445/645 *Intro to Database Systems*, Fall 2024 (chosen over a later term for its audio and structure). The exact byte layouts, counting rules and formats come from the spec; the lectures gave us the mental model behind them: why a storage layer is built this way, and how to think about each piece before writing it.

**1. Lecture 03, "Database Storage: Files & Pages".**
- *Thinking of a page as a slot array.* The lecture's picture of a slotted page (slots growing from the front, tuples from the back, a record id of page + slot) made the spec's data page (§3) easy to reason about. It also made one rule obvious: because a rid names a slot and not a byte offset, we never renumber or reuse slots, and a delete only marks its slot dead (§3 Rules, §6).
- *How to find free space.* The lecture's comparison of a linked list of pages with a page directory gave us the right question to ask: how many pages must an insert touch to find room? That is why we chose the directory (§4): an insert reads directory pages only (§5), and the directory also gives the scan its order.
- *What a tuple header is for.* The lecture's tuple carries a header with a null bitmap. Knowing what that header is for told us we could drop it: the project has no NULLs and the schema already gives each field's type, so our records have no header at all (§2).

**2. Lecture 06, "Memory & Disk I/O Management".**
- *The DBMS owns its memory.* The lecture's argument that the database, not the operating system (no mmap), should decide which pages stay in memory is the mindset behind our pool: it is the only place page bytes live, so "at most B pages" and every counter stay under our control and deterministic (§7).
- *Pins and dirty pages as the pool's contract.* Thinking of a pin as "do not take this frame" and a dirty flag as "write this back first" turned the spec's counting rules into a short table we could test line by line (§7).
- *Why replacement policies fail.* The lecture's sequential-flooding example (repeated scans of a file larger than the pool make every LRU access a miss) gave us the intuition to read our own numbers: it is the explanation for trace 3 in `REPORT_M1.md`, where LRU and CLOCK (both built to the spec, §7) behave so differently.
