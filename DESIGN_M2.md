# DESIGN_M2 — execution engine

The execution engine (`modules/exec`, plus the `exec` tool in `apps/`) takes a physical plan in JSON, builds a tree of operators, and runs it through our own buffer pool. The whole plan holds at most B pages at any moment, where B is the `--frames` value. This document covers:

- how every operator keeps to that budget, and the rule that divides B;
- the page accounting for sort and hash join;
- the sort variant;
- the long equal-key run strategy of sort-merge join;
- the hash join's overflow strategy.

The measurements are in `REPORT_M2.md`. A line-by-line check of the spec is in `modules/exec/SPEC_AUDIT.md`.

Contents:
1. The iterator model and the counters
2. How memory is counted
3. The rule for dividing B
4. How each operator keeps to its frames
5. Page accounting: external sort
6. Page accounting: Grace hash join
7. Sort variant: rewrite
8. Sort-merge join: a run of equal keys longer than B
9. Hash join: partition overflow
10. Other decisions
11. Readings

## 1. The iterator model and the counters

Every operator has the same interface (§M2.1): `open()`, `next(tuple) → true | END`, `close()`, and `counters()` with `tuples_out`, `comparisons`, `page_reads` and `page_writes`. A plan is a tree of operators; calling `next` on the root pulls one tuple up through the tree.

**Each node counts only its own work.**
- The base class `Operator` writes `open`, `next` and `close` once. Each one notes the pool's read/write counters before and after the operator's own work (`doOpen`, `doNext`, `doClose`), then subtracts what its child calls did inside it. So no operator can forget to count its pages.
- The trace prints, for each node:
  - its own `tuples_out` and `comparisons`;
  - the page reads and writes of the node **plus everything below it**.

  That way a join's line equals its textbook cost. In §M2.5's example the hash join line shows `reads=96` (24 + 24 for its inputs, 48 for probing). `run --json` totals are the plain sum over all nodes.
- **Comparisons:**
  - one per call of a tuple comparator, however many key columns it checks;
  - one per tuple a filter examines;
  - one per pair in a nested loop;
  - one per key comparison with a hash-table entry.

  The hash computation itself is not a comparison.

**Checks happen while the plan is built, before anything is opened.** Every name, type and schema check runs in the operators' constructors (`buildExecutablePlan`). These include: an unknown table or column, INT against VARCHAR, a duplicate qualified name in a join, and a non-equality condition for sort-merge or hash join. So a bad plan is rejected before any page is read or temporary file made. For the user, building and opening are one step, which is §M2.2's "rejected at open, before any execution work begins". A plan whose JSON is malformed is rejected even earlier, by the plan reader, with a message naming the node.

## 2. How memory is counted

**Every page an operator keeps in memory is a frame of the buffer pool.**
- **Pages read from a file** are pinned frames.
- **Tuples kept in a C++ container** live in a `BoundedTupleBuffer`. Examples are a sort's pass-0 run, a hash table, a nested loop's block, and a sort-merge join's run of equal keys. Every page the buffer needs is a frame *reserved* in the pool (`BufferPool::reserveFrames`), so the pool cannot use it for anything else.
  - The buffer counts tuples the way a slotted page fills: a tuple costs its encoded length plus a 4-byte slot. It goes on the last page if it fits in that page's 4088 bytes, else it starts a new page. So 13-byte records give 240 per page, the same as a real page.
- **So "at most B pages in memory" holds literally**: pinned frames plus reserved frames can never exceed the pool's B frames. If an operator tried to take more, the pool would refuse with an error instead of silently going over.

**How we know the largest number held** (§M2.6 measurement 5): the pool updates a peak counter on every pin and every reservation. `ExecutablePlan::open()` resets it and `maxFramesHeld()` reads it; `exec trace --json` prints it as `max_frames_held`.

**After close:** `ExecutablePlan::close()` checks that no page is still pinned and no temporary file is alive, and throws if one is. Every operator holds pages and files only in members that release them when destroyed, so even a plan stopped by an error releases everything.

## 3. The rule for dividing B

§M2.1 leaves the rule to Me. I have used a **phase rule**:
- **Frame Lifetime:** An operator holds buffer pool frames only while actively executing.
    
- **Concurrent Children:** During any phase, an operator grants each concurrently running child strictly its minimum required frames, keeping the remaining frames for itself.
    
- **Exclusive Children:** If an operator opens a child while holding no active frame assignments, that child receives all frames available to the operator.

Two runtime rules go with it:
1. A child is opened only just before it is consumed, and closed as soon as it is used up.
2. A blocking operator holds no frames while it opens a child. For example, union writes its sorter's buffer as a run before opening its right input.

`divideFrames(plan, B)` computes every node's share from the plan and B alone: no data sizes, no addresses. So the same plan at the same B always gets the same shares and the same counters. M4 calls the same function to estimate costs with exactly these frames.

Each node has an **open need** (frames for its work inside `open`) and a **stream need** (frames while `next` is called); it needs the larger of the two. s(c) and o(c) are a child's stream and open needs.

| Operator                               | Open need                               | Stream need                                                                  | Frames it keeps (`ownFrames`)      |
| -------------------------------------- | --------------------------------------- | ---------------------------------------------------------------------------- | ---------------------------------- |
| seqscan                                | 0                                       | 1 (one data page)                                                            | 1                                  |
| filter, project, limit, rename         | o(c)                                    | s(c)                                                                         | 0 (one tuple)                      |
| union_all                              | max(o(L), o(R), 1 + s(R))               | max(s(L), need(R)): the right input is opened after the left ends            | 0                                  |
| sort; distinct / group_by in sort mode | max(1 + s(c), 3, o(c))                  | 2 (final merge)                                                              | open − s(c) = pass-0 run           |
| distinct / group_by in hash mode       | max(1 + s(c), o(c))                     | 3 (table + reader; an overflowing partition is sorted)                       | open − s(c) = table / P            |
| union, intersect, except               | max(1 + max(s(L), s(R)), 3, o(L), o(R)) | 2 (both sorts' final merges)                                                 | open − max(s(L), s(R))             |
| sort_merge_join                        | as above                                | **3** (both final merges + 1 page for a run of equal keys, §M2.4)            | as above                           |
| hash_join                              | max(1 + max(s(L), s(R)), o(L), o(R))    | 3 (table + reader; an overflowing pair needs a block + 2 readers)            | open − max(s(L), s(R)) = **P**     |
| nested_loop_join                       | max(o(L), 1 + s(L), o(R), 1 + s(R))     | 1 + s(L) + need(R) (block + streaming outer + the inner, reopened per block) | stream − outer − inner = **block** |

Worked examples at B = 4:
- `sort(seqscan)`: pass-0 runs of 3 pages, merges of 4.
- `nested_loop_join(seqscan, seqscan)`: a block of 4 − 1 − 1 = **2** = B − 2.
- `hash_join(seqscan, seqscan)`: **P = 3** = B − 1, probe with 4.
- `sort_merge_join(seqscan, seqscan)`: each sort makes runs of 3, merges with 4, then the join merges with 4.

**Every plan fits at B = 4** (§M2.8, and §M2.1 "as small as 4 during grading"). Two operators can meet an input that needs more frames than are left beside them:
- a nested loop join whose inner input is a sort or whose outer input is another join, as in a three-table chain `nested_loop_join(nested_loop_join(R, S), T)`;
- `union_all` whose right input is blocking, as in `sort(union_all(X, sort(hash_join(…))))`.

Such an operator first runs that input alone inside `open()` and writes it to a temporary run, then reads the run with 1 frame (`FrameShare::writeOuter` / `writeInner`). That is "genuinely required" materialisation (§M2.1).
- It is planned only when the unwritten form would leave no frame for a parent at that B. At a large B nothing is written.
- With it, every operator streams with at most 3 frames and opens with at most 4, so every plan fits in 4.
- It costs one write of that input. A sort used as an inner input is then done once instead of once per block.

## 4. How each operator keeps to its frames

**Pipelined operators**
- **seqscan:** pins one page at a time: the directory page, then each data page. It reads each directory page once, notes its entry count, and visits data pages by number (storage fix, 2026-11-07). Holds 1 frame.
- **filter, project, rename, limit:** hold one tuple, no page.
  - `limit 0` never opens its child.
  - `limit n` closes its child after the n-th tuple, so it really stops pulling.
- **union_all:** reads the left input, closes it, then opens the right one (or reads the right one's run, §3).

**Sorts**
- **sort:** pass 0 fills a buffer of `ownFrames` pages, sorts it with `std::sort` (one run that fits in memory) and writes it. Merge passes use the open share; the final merge streams with the stream share (§5).
  - The page a run is written to is one of the buffer's frames: the buffer hands one frame to the writer, and tuples move one by one from the buffer onto the page. So the data in memory never grows.
  - An input that fits in the buffer and in the final pass's frames is never written.
- **distinct / group_by, sort mode:** sort the input (on all columns, or on the grouping keys), then skip adjacent equal tuples or fold each group's rows. A group's state is one row: its key, then per aggregate COUNT, SUM, the AVG total and count, MIN or MAX. No keys means one group, with no sort.

**Hash mode**
- **distinct / group_by, hash mode:** a hash table of up to `ownFrames` pages of entries.
  - While it fits, everything happens in memory.
  - When it would overflow, the table is written to one spill run. That run and the rest of the input are split into P partitions by h1. Each partition is then rebuilt in a table by h2.
  - A partition with more distinct values than its table can hold is external-sorted instead.

**Two-input sorts**
- **union:** both inputs go through **one** sorter (left, then right), then adjacent equal tuples are skipped. Cost: sort(L + R).
- **intersect, except:** sort the left input to runs on disk (holding no frame afterwards), then the right one. Then both sorts stream their final merge side by side, at least 1 run each, into one merge: cost sort(M) + sort(N). Set semantics: each output tuple once.

**Joins**
- **nested_loop_join:** a block of `ownFrames` pages of the outer, then the whole inner streamed past it, every pair tested. The inner is closed and reopened for each block. §3 covers an input that needs more than one frame.
- **sort_merge_join:** §8.
- **hash_join:** §6 and §9.

All three joins output the left tuple's values, then the right's. The output schema is the left schema followed by the right one.

## 5. Page accounting: external sort

For N input pages:

1. **Pass 0** writes ceil(N / r) runs of r pages, where r = the sort's run frames. That is **N writes**; the N input reads are charged to the input's own node.
2. **Each merge pass** merges (mergeFrames − 1) runs at a time, with 1 frame for the output page: **N reads and N writes**. Merging stops when the final pass can take every run at once.
3. **The final pass** merges while the parent calls `next`: **N reads and no writes**.

Passes = pass 0 + every merge pass, the final one included. §M2.3's formula is `1 + ceil(log(ceil(N/B)) / log(B−1))`. With the rewrite variant (§7) the total is

> N (input) + N (pass-0 writes) + 2·N per middle pass + N (final reads) = **2·N·(passes − 1) + N**,

which is M4's sort formula.

**The spec's example (N = 8, B = 3):**

| Pass | Work | Reads | Writes |
|---|---|---|---|
| 0 | read the 8 input pages, write runs of 3, 3, 2 pages | 8 | 8 |
| 1 | merge runs 1 and 2 into one 6-page run, **copy** run 3 | 8 | 8 |
| 2 | merge the last 2 runs, streaming to the parent | 8 | 0 |
| **Total** | 3 passes | **24** | **16** |

24 + 16 = 40 = 2·8·(3 − 1) + 8. The unit test `external_sort_tests.cpp` reproduces this table exactly.

**Two consequences of our page-accounting choices:**
- **Every run page is written once and read once per pass.** A run writer writes each full page to disk at once. A finished run's pages are dropped from the pool, so the next pass reads them from disk and never hits a cached copy. That way measured I/O equals the formula, instead of being lowered by cache hits.
- **Inside a plan, pass 0 has B − 1 frames, not B, because the input's scan holds one.** So the runs are ceil(N/(B−1)), and the pass count can be one more than the formula. For example, N = 12, B = 4 gives 3 passes, not 2. `REPORT_M2.md` reports both values.

## 6. Page accounting: Grace hash join

With M = left (build) pages and N = right pages, P = `ownFrames` = B − 1 when both inputs are scans.

| Phase | Work | Reads | Writes | Charged to |
|---|---|---|---|---|
| Partition R | scan R, write each tuple to partition h1(key) mod P | M | — | the left child (scan) |
| | write the P partition pages as they fill | — | ≈ M | the join |
| Partition S | the same for S, with the same h1 and P | N | ≈ N | scan / join |
| Probe | for each i: read R_i into a table by h2, read S_i and probe | ≈ M + N | — | the join |
| **Total** | | **2(M + N)** | **M + N** | = **3(M + N)** |

The join's trace line (its own + its inputs) shows reads 2(M + N) and writes M + N. With M = N = 24 that is §M2.5's `reads=96 writes=48`.

- **"≈":** each partition's last page is usually only partly full, so the partitions take a little more than M + N pages. Up to P extra pages per side; large when P is big next to the input.
- **Frames:**
  - partitioning: P writer pages + the input's page = B;
  - probing: the table gets (stream share − 1) pages + 1 reader for S_i.
- **Matching tuples share a partition:** both sides hash the key columns in pair order with the same h1 seed. Each key pair has the same type (checked at build), so equal keys have equal bytes.
- **The hash is our own** (DECISIONS 2026-11-07): FNV-1a 64 over the record codec's bytes, with a seed (h1 = 1, h2 = 2) and a final mix. Partition sizes are then the same on every machine, unlike `std::hash`.
- Both inputs are always partitioned, even when one is empty, and every partition pair is probed, so the cost stays 3(M + N). The left input is always the build side; the optimizer chooses which input goes left.

## 7. Sort variant: rewrite

In a merge pass, a leftover group of one run is **copied** (rewritten), not carried forward untouched. So in the spec's example pass 1 is 8 reads / 8 writes, and the totals are 24 / 16.

Why: M4's cost model uses `2·M·(passes − 1) + M` (§M4.4), which assumes rewrite. With rewrite, measured sort I/O and M4's estimate agree with no correction term. Carry would save a few pages (22 / 14 in the example) for the same amount of code, but every estimate would then need a special case. The spec accepts either if it is stated.

## 8. Sort-merge join: a run of equal keys longer than B

**The algorithm:** both inputs are sorted on the join key with the external sort, the left first, then the right. Both sorts then stream their final merge into the join's merge ("using your sort operator", §M2.3), which follows the spec's pseudocode exactly:
- advance the smaller key;
- on equal keys, collect the **run of right tuples** with that key;
- pair every left tuple of the same key with every tuple of that run, rewinding to the start of the run for each left tuple.

The spec's example (R.b = [1, 2, 2, 3], S.b = [2, 2, 4]) gives 4 rows; advancing instead of rewinding would give 2.

**The run of equal right keys is kept in 1 page** (a counted buffer).
- If it outgrows that page, it is **spilled to a temporary run**: the page's frame is handed to a run writer, so the frames held do not grow, and the rest of the run follows.
- Each matching left tuple then rereads the spilled run from disk: its pages once per left tuple. §M2.3 explicitly accepts spilling the inner run ("spilling the inner run to a temp file is acceptable; assuming it fits is not").
- A run of any length works. It is tested with 1200 × 1200 equal keys (5 pages per side) at B = 4.

**Frames while merging:** 1 page for the run, the rest split between the two final merges (at least 1 run each): **3 at least**, as §M2.4 says ("3 for the merge").

**Cost:** the sorts' reads and writes, plus rereads of spilled runs. §M2.3's formula `sort(M) + sort(N) + M + N` counts a merge read of each sorted input. Ours has no separate merge read: the final merge passes *are* the join's reading of the inputs, so they are already in sort(). On the other side:
- the two final merges share B, so a sort may need one more merge pass;
- an input that would fit in memory is still written once (both sorts must hold no frame while the other runs);
- the merge stops as soon as one input ends.

`REPORT_M2.md` measurement 1 shows the gap in both directions and explains it.

## 9. Hash join: partition overflow

**Choice (b) of §M2.3: block nested loop on that one partition pair** (user's choice). There is no re-partitioning.

- **Detection, before building:** a left partition R_i whose page count is larger than the table's frames (stream share − 1) cannot fit. Its page count is known when partitioning ends, so the table can never grow past its frames. A partition that fits is built normally; the table counts pages the same way run pages fill, so it fits exactly.
- **The fallback:** R_i is read in blocks of (stream share − 2) pages. For each block, S_i is read again from the start and every pair is compared, one comparison per pair as in the nested loop join. Frames: block + R_i reader + S_i reader = the stream share. Cost of that pair: **R_i + ceil(R_i / block) · S_i** reads.
- **Why not re-partition:** with the skew of §M2.6 measurement 4 (one key value in 40 % of the rows on both sides), every copy of that key hashes to the same partition with *any* hash function. Rehashing can never split it, so (a) would always need (b) behind it anyway. (b) handles it at once, with a cost formula we can state.
- **Reported:** `exec trace --json` gives the partition count, the pages of every left and right partition, and the list of overflowed partitions (`"overflow_strategy": "block_nested_loop"`).

## 10. Other decisions

- **Plan format:** the handouts' format (§M2.2), with the student choices from DECISIONS.md:
  - `limit` uses `n`;
  - aliases and self-joins use our physical `rename{to, child}`;
  - `mode` is required on distinct and group_by.

  Aggregate columns are named `count_star`, `avg_R_salary`, … with no qualifier.
- **Types:**
  - INT against VARCHAR is an error in every operator.
  - VARCHAR compares byte-wise on UTF-8.
  - AVG's result is a REAL (a `double`), printed with exactly 4 decimals. REAL compares with INT as a number, so `HAVING AVG(x) > 50000` works. SUM and AVG need INT.
  - With no grouping keys, an empty input gives one row with COUNT 0; asking for another aggregate there is an error.
- **Temporary files:** one file per run or partition. Destroying it deletes the file.
- **Storage changes made for exec** (each in storage STATUS.md):
  - frames reserved for tuple buffers;
  - the scan reads each directory page once;
  - REAL on run pages;
  - at most 256 files open at once: a hash join at B = 256 keeps 510 partition files, more than Windows allows, so storage closes the least recently used file.
- **The `exec` tool:**
  - `exec run` / `exec trace` with `--db`, `--plan`, `--frames` and `--json`, in §M2.5's formats;
  - `run --json` writes rows as they come out of the plan, not collected first;
  - exit codes: 1 for a user error (printed with `formatError`), 2 for a bad command line, 3 for an internal error.

## 11. Readings

Andy Pavlo's CMU 15-445/645 *Intro to Database Systems*, Fall 2024 ( is base for all concepts used here)

- **Graefe, *Volcano — An Extensible and Parallel Query Evaluation System*, IEEE TKDE 1994.**
  - The open/next/close iterator protocol is how every operator here is written, including that a parent drives its children and never reaches past them.
  - The paper's separation of "what an operator does" from "how it is called" led to the base class that wraps every `open`/`next`/`close` once to do the page counting, so no operator counts its own I/O.
  - Its distinction between pipelining and materialisation is the basis of the phase rule. Pipelined operators hold one tuple; only blocking operators hold frames, and only while they are working.
- **Shapiro, *Join Processing in Database Systems with Large Main Memories*, ACM TODS 1986.**
  - The Grace hash join's two phases and their cost: partition both inputs with one hash function, then build and probe each pair with a second one. That is why h1 and h2 have different seeds; a table built with the partitioning hash would crowd every key of a partition into a few buckets.
  - Its discussion of partitions that do not fit in memory shaped the overflow choice in §9: detect overflow from the partition's size before building, and fall back to a nested loop on that pair rather than rehash, because a single heavy key cannot be split.
- **Ramakrishnan and Gehrke, *Database Management Systems*, ch. 13 (external sorting) and ch. 14 (joins).**
  - The external merge sort's pass count and cost, and the carry/rewrite question in §7.
  - The block nested loop's B − 2 block with one input and one output page.
  - The sort-merge join's handling of duplicate keys by rewinding the inner input, which is the spec's pseudocode.
