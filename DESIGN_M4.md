# DESIGN_M4 — Query optimizer

The optimizer takes a **logical plan** (what to compute) and returns a **physical plan** (how to compute it): which join algorithm, in which order, with which filters where. The physical plan is the exact JSON that the Module 2 executor (`exec`) runs, with no editing.

It works in five steps:

1. **Statistics.** `ANALYZE` measures each table exactly and stores the numbers in the catalog (section 1).
2. **Estimates.** From the statistics, a **selectivity** (the fraction of rows a condition keeps) and a row count for every node (section 2).
3. **Rewrite rules.** Seven rules move filters and projections to cheaper places in the logical plan (section 3).
4. **Costs.** An I/O cost model counts the pages each operator reads and writes, as a function of the memory budget B (section 4).
5. **Join order.** A dynamic-programming search over left-deep join orders picks the cheapest order and, for each join, one of the three join algorithms (section 5).

Section 6 lists my decisions, the places where we depart from a spec number (with the working), and the known limits. The measured behaviour of all of this is in `REPORT_M4.md`.

Code: `modules/opt/` (library `db_opt`), the tool `apps/opt_cli.cpp`. Commands: `opt analyze`, `opt plan`, `opt explain`, `opt dp-table` (§M4.6).

---

## 1. Statistics layout in the catalog

### What is stored

ANALYZE stores one `TableStats` record per table (storage's type, `db/storage/table_info.h`):

| Field | Meaning |
|---|---|
| `nTuples` | live rows (§M4.1 `n_tuples`) |
| `nPages` | data pages, **not** counting directory pages (§M4.1 `n_pages`) |
| `width` | average record length in bytes, a `double` |
| `columns` | one `ColumnStats` per column, in schema order |

Each `ColumnStats` holds:

| Field | Meaning |
|---|---|
| `ndv` | **number of distinct values** in the column |
| `min`, `max` | lowest and highest value |
| `histogram` | the buckets, in ascending order; each bucket has `lo`, `hi`, `count` (rows in the bucket) and `ndv` (distinct values in the bucket) |

### Where it lives

The statistics live in the Module 1 catalog file `catalog.db`, next to the table records. All statistics of one table are encoded as one blob and cut into "kind 2" catalog records of at most 4084 bytes each, so any bucket count fits. `DESIGN_M1.md` ("Statistics (room for M4)") describes the byte layout. The width is stored as its exact 8 IEEE-754 bytes, so the width ANALYZE computed is bit for bit the width every later command reads.

Opt reads storage's `TableInfo` directly; there is no wrapper type. Row and page counts always come from the catalog's own counts (`numTuples`, `numPages`), which storage keeps exact on every insert, never from `stats.nTuples`, which would be stale after a later insert. So even a table that was never analyzed has exact row and page counts.

### How ANALYZE computes them

- **Rows, pages:** the catalog's exact counts.
- **Width:** the total encoded length of all records / rows. Example: our `Emp` table, 10,000 rows, width 26.0 bytes.
- **Per column:** the column's values are sorted with the **Module 2 external sort** (§M4.1: the column may not fit in memory), then one pass over the sorted values counts ndv, takes min and max, and cuts the buckets. Only O(k) values are kept in memory, never the column.

**"Scans the table once" (§M4.1).** One heap scan feeds the sorts of several columns at the same time. Each sort needs 2 frames and the scan 1, so one scan can serve up to (B − 1) / 2 columns. After the scan, every sort but the first one writes its buffered values to a run on disk (`writeBufferedRun`, exec's public sorter), then the sorts finish one at a time with all B frames. So:
- at B = 32 one scan serves up to 15 columns (every table in our workload is read once);
- at B = 3 or 4 there is one scan per column.

The statistics are the same either way (tested at B = 32, 5 and 4).

**Histogram: equi-depth, k buckets (default 20, `--buckets`).** **Equi-depth** means each bucket holds about the same number of rows, so buckets are narrow where values are dense. The cutting rule:
- bucket i (from 0) ends after sorted position `(i + 1) × n / k`, in whole numbers;
- a run of equal values is never split across two buckets; when a run passes a cut point, the bucket ends at the end of the run, and the next bucket ends at the first cut point still ahead;
- so the buckets never overlap and are never empty, and there may be fewer than k.

Example (k = 2, values `1,1,1,1,1,1,2,3,4,5`): the first cut is after position 5, inside the run of six 1s, so bucket 0 is `[1,1]` with count 6, ndv (number of distinct values in the bucket) 1, and bucket 1 is `[2,5]` with count 4, ndv 4.

An empty column gets ndv 0, no buckets and a typed placeholder for min/max.

**Running ANALYZE twice changes nothing.** The cut points are whole-number positions, the sort is deterministic, the width is stored exactly, and the new record replaces the old one. The `catalog.db` file is byte-identical after a second ANALYZE (tested: `analyze_tests.cpp` "ANALYZE twice changes nothing", and through the CLI).

---

## 2. Selectivity formulas and assumptions

**Selectivity** is the fraction of input rows a condition keeps, a number in [0, 1]. Output rows of a node = input rows × selectivity.

### Single conditions on a column

| Condition                    | Formula we use                                                                                                                                                                                                                                                    | Assumption                                                                                      |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| `c = v`                      | Find the bucket holding v: `count(bucket) / (n × ndv(bucket))`, n = rows in all buckets. v in no bucket (also in a gap between two buckets) → 0. <br><br>No histogram → `1 / ndv(c)`.                                                                             | **Uniformity inside a bucket**: every distinct value of the bucket has the same number of rows. |
| `c < v`, `c > v`             | Whole buckets on the correct side, plus a linear fraction of the bucket holding v: `(v − lo) / (hi − lo)` for `<`, `(hi − v) / (hi − lo)` for `>`. No histogram → `[min, max]` as one bucket. VARCHAR (no subtraction on strings) → half of the bucket holding v. | Values spread evenly across a bucket's range.                                                   |
| `c <= v`, `c >= v`           | The range part + the equality part, capped at 1.                                                                                                                                                                                                                  | As above.                                                                                       |
| `c <> v`                     | `1 − sel(c = v)` (§M4.2).                                                                                                                                                                                                                                         |                                                                                                 |
| `v op c`                     | Flipped to `c op' v` (`5 < c` → `c > 5`).                                                                                                                                                                                                                         |                                                                                                 |
| column vs column, same table | 0.1                                                                                                                                                                                                                                                               | No information.                                                                                 |
| literal vs literal           | 1 or 0 (it is true or false).                                                                                                                                                                                                                                     |                                                                                                 |

**Worked example (§M4.2, our golden test).** Emp: 10,000 rows, `ndv(did) = 50`, `age` uniform in 20–70.
- `did = 7`: 1/50 = 0.02 → 200 rows.
- `age > 60`: (70 − 60) / (70 − 20) = 10/50 = 0.20 → 2,000 rows.

**Where uniformity breaks.** Values are whole numbers, but the formula treats the range as continuous. On a bucket [6, 10], `a < 8` estimates (8 − 6) / (10 − 6) = 0.5 of the bucket. The bucket actually holds 5 whole values (6–10), and 2 of them (6, 7) are below 8, so the right fraction is 0.4. For §M4.2's example, a discrete INT formula would give 10/51, not 10/50. We keep the spec's continuous form.

```
 condition:  a < 8,     one bucket holds values 6 … 10

 how we estimate (continuous line):
   6 ───────────────── 8 ───────────────── 10
   └───── (8 − 6) ─────┘
          ─────────  =  2 / 4  =  0.5 of the bucket

 what is really there (whole numbers only):
   [6]   [7]  │ [8]   [9]   [10]
    ✔     ✔   │  ✘     ✘     ✘        →  2 of 5 values  =  0.4 of the bucket

 The formula treats the bucket as a smooth line; INT values are separate points.
 For §M4.2's age example this is why 10/50 is really 10/51.
```

### Combining conditions

| Condition | Formula | Assumption |
|---|---|---|
| `p AND q` | `sel(p) × sel(q)` | **Independence**: knowing p is true tells nothing about q. |
| `p OR q` | `sel(p) + sel(q) − sel(p) × sel(q)` | Independence. |
| `NOT p` | `1 − sel(p)` | |

The conditions are combined in the order they are written. Example: `did = 7 AND age > 60` = 0.02 × 0.20 = 0.004 → 40 rows.

**Where independence breaks (`REPORT_M4.md` §4).** In our data, `Cust.zip` decides `Cust.city` (zips 0–49 are in City00). For `city = 'City00' AND zip < 50`, both conditions keep the same 5 % of the rows, so the true selectivity is 0.05. The product gives 0.05 × 0.05 = 0.0025, so the estimate is 82 rows where there are 1,618 (20 times fewer). Because of that wrong guess, the database sticks with a slow "nested loop" method instead of switching to a faster "hash join," forcing it to do 1.85 times more disk work (11,064 operations instead of 5,966 when limited to 4 memory slots).

### Joins

| Join condition                 | Output rows                                                                                                                |
| ------------------------------ | -------------------------------------------------------------------------------------------------------------------------- |
| `R.a = S.b`                    | `n(R) × n(S) / max of (ndv(R.a),ndv(S.b))` (§M4.2). <br>One ndv unknown → the known one; <br>none known → selectivity 0.1. |
| any other join condition       | selectivity **0.1** (§M4.2).                                                                                               |
| several conditions on one join | the product (independence again)                                                                                           |
| a cross product                | `n(R) × n(S)`                                                                                                              |

When estimating how many rows will match in an **equality join** between two tables, the formula assumes **containment**—meaning every value from the side with fewer **distinct values (`ndv`)** also exists on the other side. For example, joining 10,000 employees (`Emp`) with 50 departments (`Dept`) on department ID (`did`) calculates `(10,000 × 50) ÷ max(50, 50) = 10,000` rows, which is accurate because `did` is a **key** of `Dept` (every employee belongs to a valid department). To get these `ndv` counts, the database simply takes the original numbers recorded on the raw table during its initial **`ANALYZE`** scan and carries them upward through every step of the query unchanged.

### Other operators

| Node             | Rows                                                  | Width                                                        |
| ---------------- | ----------------------------------------------------- | ------------------------------------------------------------ |
| scan             | catalog rows                                          | table width                                                  |
| select           | input × selectivity                                   | input                                                        |
| project          | input                                                 | sum of the kept columns' widths                              |
| group_by         | min(input, product of the keys' ndv); <br>no keys → 1 | keys + aggregates (count/sum 4, avg 8, min/max the column's) |
| distinct         | min(input, product of every column's ndv)             | input                                                        |
| limit n          | min(n, input)                                         | input                                                        |
| union, union_all | left + right                                          | left                                                         |
| intersect        | min(left, right)                                      | left                                                         |
| except           | left                                                  | left                                                         |

A column's width: INT is 4 bytes; the VARCHAR columns share the rest of the table's average width equally. Example (our test table): width 30 bytes, 4 INT columns and 1 VARCHAR → the VARCHAR is 30 − 16 = 14 bytes.

### Rounding

- **Rows:** `ceil(rows)`, never below 1 (§M4.2). Before the ceiling we take off a tiny amount (10⁻⁹ × the value), because floating point can make a whole number slightly too big: (0.1 × 3) × 10 = 3.0000000000000004, which a plain `ceil` would turn into 4.
- **Pages:** `ceil(rows × width / 4096)`, with the same guard (§M4.2). This is how a projection that drops columns makes later operators cheaper.

### Defaults when a table has no statistics (§M4.9)

All defaults are in one file, `modules/opt/include/db/opt/defaults.h`:

| Case | Default |
|---|---|
| `c = v` | 0.1 |
| `c < v`, `c > v`, `c <= v`, `c >= v` | 1/3 |
| `c <> v` | 0.9 |
| a join condition other than `R.a = S.b` | 0.1 (§M4.2) |
| `R.a = S.b` with no ndv known | 0.1 |
| group_by / distinct on a column with unknown ndv | no reduction (the input row count) |
| width | data pages × 4096 / rows; an empty table: the schema width (INT 4, VARCHAR(n) 2 + n) |
| histogram buckets | 20 |

These are the classic System R guesses. Row and page counts never need a default, because the catalog keeps them exact. So a never-analyzed table still has exact sizes and only per-column guesses (tested: `planner_tests.cpp` "Optimized and unoptimized plans give the same rows in exec, with and without statistics").

---

## 3. Rewrite rules with preconditions

The rewrite rules reshape the logical plan before any cost is computed. Each rule has a **precondition**: a check that must be true before the rule may fire. A rule that fires when its precondition is false gives **wrong answers**, not slow ones (§M4.3).

| # | What the rule does | Only when | What goes wrong otherwise |
|---|---|---|---|
| 1 | Splits a filter `p AND q` into two filters, `p` and `q`. | Always safe. | — |
| 2 | Moves a filter below a join, into one input. | The filter uses columns of **one input only**. | `e.age > d.budget` moved to Emp's side cannot run there: Emp's rows have no `d.budget`. |
| 3 | Turns "cross product + filter" into a join. | The filter uses both inputs and is an `=`. | — (only a speed-up; it makes a comma join as good as JOIN ON). |
| 4 | Moves a filter below a rename (names rewritten) or a union. | For a union: the filter goes into **both** branches. | Filter only one branch, and the other branch's rows come through unfiltered. |
| 5 | Moves a filter below a group_by. | The filter uses **grouping columns only**. | `COUNT(*) > 5` below the group_by: the count does not exist yet (this is HAVING vs WHERE). |
| 6 | Adds projections below join inputs to drop unneeded columns. | Never drop a column that something above still uses. | Drop `e.did` below `join (e.did = d.did)`, and the join cannot compare it. |
| 7 | Removes a projection that changes nothing; merges filters that sit one on top of another. | The projection keeps every column, in the same order. | Remove a projection that reorders columns, and the output columns come out in the wrong order. |

**The §M4.3 example, before and after, with the rule behind each change:**

**Before** (rule 1 has already split the WHERE clause into three selects):

```
project [e.name]
  select e.did = d.did          ← uses both tables, is "="  → becomes the join (rule 3)
    select e.age > 60           ← uses only Emp             → moves down to Emp (rule 2)
      select d.budget > 1000    ← uses only Dept            → moves down to Dept (rule 2)
        cross
          rename e (scan Emp)
          rename d (scan Dept)
```

**After:**

```
project [e.name]
  join e.did = d.did            ← the cross became a join (rule 3)
    project [e.name, e.did]     ← keeps only the columns needed above (rule 6)
      select e.age > 60         ← moved down to Emp's side (rule 2)
        rename e (scan Emp)
    project [d.did]             ← keeps only the join column (rule 6)
      select d.budget > 1000    ← moved down to Dept's side (rule 2)
        rename d (scan Dept)
```

### How the rules run: two walks down the tree

The textbook way is to apply every rule again and again until nothing changes. We do something simpler: **two walks from the top of the tree to the bottom**. Each walk carries a list down the tree and, at every node, decides what to leave there.

- **Walk 1 moves filters down** (rules 1–5, and rule 7's merge).
- **Walk 2 removes unneeded columns** (rule 6, and rule 7's removal).

Two walks are enough. Filters only ever move down and columns only ever get removed, so one walk of each kind puts everything in its final place.

We follow the §M4.3 example through both walks:

```sql
SELECT e.name FROM Emp e, Dept d
 WHERE e.did = d.did AND e.age > 60 AND d.budget > 1000;
```

#### Walk 1: move each filter as far down as it can safely go

First, the WHERE clause is split at every AND (rule 1). That gives three separate conditions: `e.did = d.did`, `e.age > 60`, `d.budget > 1000`.

Then the walk carries these conditions down the tree. At each node it checks every condition against this table:

| Node it reaches | What happens to the condition | Rule |
|---|---|---|
| join or cross, condition uses **one side only** | moves into that side | 2 |
| join or cross, condition uses **both sides** and is `=` | becomes the join's condition (a cross becomes a join) | 3 |
| rename (an alias of a subquery) | moves through it; column names are rewritten (`t.did` → `e.did`) | 4 |
| union, union_all | is copied into **both** branches | 4 |
| group_by, condition uses **only grouping columns** | moves below it | 5 |
| project, distinct, sort, limit, intersect, except | **stops** here | — |

A condition that stops is left as a filter at that spot. With rule 7, several filters at one spot are merged into one.

In the example, all three conditions reach the cross product of Emp and Dept:
- `e.age > 60` uses only Emp, so it moves down to Emp's side.
- `d.budget > 1000` uses only Dept, so it moves down to Dept's side.
- `e.did = d.did` uses both sides and is an equality, so the cross becomes `join (e.did = d.did)`.

#### Walk 2: remove the columns nobody needs

This walk carries down the list of **columns that something above still needs**. Below each input of a join it inserts a projection that keeps only those columns, plus the columns the join itself compares (rule 6).

In the example:
- The query returns only `e.name`.
- The join compares `e.did` and `d.did`.
- So Emp's side gets `project [e.name, e.did]`, and Dept's side gets `project [d.did]`.

Two safety rules:
- A projection is added only if it removes at least one column and keeps at least one.
- Below a distinct or a set operation (union, intersect, except) nothing is removed, because those compare whole rows.

Rule 7 then deletes any projection that keeps every column in the same order, since it does nothing.

The result is exactly the spec's "after" tree (`rewrite_tests.cpp`: "§M4.3 worked example: all rules give the spec's 'after' tree"). `opt explain --show-rewrite` prints the tree before and after, with row estimates.

### Our choices where the spec is silent

**1. A JOIN ON condition is treated like a WHERE condition.**
`FROM Emp e JOIN Dept d ON e.did = d.did AND e.age > 60` is split like a WHERE clause: `e.age > 60` moves down to Emp, and `e.did = d.did` stays as the join condition. This is why a comma join and the same JOIN ON query get the **same plan** (§M4.9). We do this only when rule 3 is on; with rule 3 off, a written JOIN is left as it is.

**2. A filter stays above an alias of a plain table.**
In `rename e (scan Emp)`, the rename only gives Emp the nickname `e`. Moving `e.age > 60` below it would change nothing, and the spec's "after" tree keeps the filter above `rename e`. So rule 4 skips a rename that sits directly on a table.

**3. Projections are added only at join inputs.**
A join's cost depends on how many pages its inputs take, so narrower rows there mean less I/O. Everywhere else, rows stream from one operator to the next without being stored, so a projection saves no I/O.

**4. Rule 5 needs at least one grouping column, and a group_by with keys.**
A group_by with no keys (`SELECT COUNT(*) FROM R`) returns one row even when its input is empty (COUNT = 0). A constant filter such as `1 = 2` uses no columns, so "uses only grouping columns" is trivially true. But moving it below would keep that one row, while above it removes it. So a filter with no columns never moves below a group_by.

**5. Filters never move below a projection: a known gap.**
Take a WHERE condition on a subquery's grouping column:

```sql
SELECT t.did, t.n FROM (SELECT e.did, COUNT(*) AS n FROM Emp e GROUP BY e.did) AS t
 WHERE t.did < 10;
```

Rule 5 allows `t.did < 10` to move below the group_by, because `did` is a grouping column. But the SQL translator always puts a projection right above a group_by, and walk 1 stops at projections. That projection keeps every column, so rule 7 removes it, but only in walk 2, after filters have stopped moving. So the filter ends up just above the group_by instead of below it.
- **The result is still correct**: no rule fires where its precondition fails.
- **But a rewrite is missed**, so rule 5 only ever fires on a HAVING condition on a grouping column.
- **The fix:** let walk 1 move a filter through a projection that keeps the filter's columns, a standard algebra rule.
- In our workload it would save no I/O (`REPORT_M4.md` §3).

**6. Deep trees work.** Both walks keep their stack use small, so trees about 1,000 levels deep are handled (a project rule since Phase 0).

---

## 4. Cost model

**What we count.** The cost of a plan is the number of **pages it reads plus pages it writes** (§M4.4). CPU work, such as comparing rows, is ignored. Reading a page from disk is far slower than comparing rows in memory, so page I/O decides which plan is faster.

Three letters appear in every formula:
- **M** = the estimated number of pages of the (left) input;
- **N** = the estimated number of pages of the right input;
- **B** = the memory budget, in pages (frames).

M and N are the input's **estimated output pages**: after any filters and projections below it, not the size of the table on disk.

### The formulas

| Operator | Cost | In words |
|---|---|---|
| seqscan | M | Read every data page of the table once. |
| filter, project, rename, limit, union_all | 0 | Rows pass straight through to the next operator; nothing is written. |
| sort | `2 × M × (passes − 1) + M` | Read the input once; every extra pass writes and reads it again. |
| | passes = `1 + ceil(log(ceil(M/B)) / log(B − 1))` | Counted with whole numbers, never a floating `log` (it can round wrongly). |
| nested_loop_join | `M + ceil(M / (B − 2)) × N` | Read the outer once, B − 2 pages at a time; read the whole inner once per such block. |
| sort_merge_join | `sort(M) + sort(N)` | Sort both inputs; the merge reads nothing extra (see below). |
| hash_join | `3 × (M + N)` | Read both inputs, write them out as partitions, read the partitions back. |
| distinct, group_by (sort mode) | `sort(M)` | Sort, then equal rows are next to each other. |
| distinct, group_by (hash mode) | M if all groups fit in memory, else `3M + 2 × table pages` | Same as exec's hash grouping (DECISIONS 2026-11-07). |
| group_by with no keys | M | One running total; no sort needed. |
| union | `sort(L + R)` | One sort of both inputs together. |
| intersect, except | `sort(M) + sort(N)` | Sort each input, then merge. |

**How a nested loop reads its inner input** (the 650 case of §M4.5, B = 32, block = 30 pages):

```
 outer: {B′,C} result, 40 pages                       inner: A, 100 pages
 ┌──────────────────────────────┬──────────┐
 │ block 1: pages 1–30          │ block 2: │
 │                              │ 31–40    │
 └──────────────────────────────┴──────────┘
        │                             │
        └── read all of A (100) ──────┼──►  pass 1
                                      └──►  pass 2: read all of A again (100)

 cost = read the outer once + one full pass over the inner per block
      =        40           +        ceil(40 / 30) = 2 × 100
      = 240   (+ 410 for producing the outer = 650)
```

**Example 1: a scan and a nested loop (§M4.5, B = 32).** A is 100 pages and B′ is 10 pages.
- **Scan A:** cost = M = **100**. Every page is read once.
- **Nested loop, B′ as the outer, A as the inner:** M = 10 (B′), N = 100 (A).
  - The block holds B − 2 = 32 − 2 = **30** outer pages. The other 2 pages are the ones the two scans are reading from (one page of B′, one of A).
  - Number of blocks = ceil(M / (B − 2)) = ceil(10 / 30) = **1**. All of B′ fits in one block.
  - A is read once per block, so once.
  - Cost = M + blocks × N = 10 + 1 × 100 = **110**.

**Example 2: a sort, and why it matches exec exactly.** Sort M = 8 pages with B = 3.
- **The formula:**
  - Pass count = 1 + ceil(log(ceil(8 / 3)) / log(3 − 1)) = 1 + ceil(log 3 / log 2) = 1 + ceil(1.58) = 1 + 2 = **3**.
  - Cost = 2 × M × (passes − 1) + M = 2 × 8 × 2 + 8 = 32 + 8 = **40**.
- **What exec does:**

  | Pass | What happens | Reads | Writes |
  |---|---|---|---|
  | 0 | Read the 8 pages; sort them 3 pages at a time into 3 runs (3 + 3 + 2 pages); write the runs. | 8 | 8 |
  | 1 | Merge 2 runs at a time (B − 1 = 2): the two 3-page runs become one 6-page run. The 2-page run has no partner, so it is **copied** to a new run. | 8 | 8 |
  | 2 | Merge the last two runs (6 + 2 pages). The result goes straight to the next operator, so nothing is written. | 8 | 0 |
  | **Total** | | **24** | **16** |

  24 + 16 = **40**, the formula's number.
- **Why the copy matters:** the formula assumes every pass reads and writes all M pages. The copy in pass 1 makes that true (DECISIONS 2026-11-06, the "rewrite" variant). If exec left the 2-page run where it was, pass 1 would read and write only 6 pages, and exec would measure 36, not 40.

### How costs add up in a plan

Every join formula already includes reading each input once. So:
- **The input is a table:** its scan is not added again. {A, B′} = 10 + 1 × 100 = **110**, not 10 + 10 + 100.
- **The input is another join:** that join's own cost is added on top. {A, B′, C} = 110 (making the outer) + 20 (reading it) + 1 × 400 (one pass over C) = **530**.

This is the only way of adding up that reproduces the §M4.5 table, which the checklist requires.

One side effect: when a table's input has a filter below it, M or N is the **filtered** size. Exec, however, rereads the whole table each time it rereads a nested loop's inner input. So a nested loop over a filtered inner input costs more than predicted (`REPORT_M4.md` §5).

### Where we differ from §M4.4, and why

§M4.4 says: if opt and exec disagree on a cost, "say which is right and fix the other". In both cases below, exec's measured I/O is right.

**Sort-merge join: we fixed the formula.** §M4.4 writes `sort(M) + sort(N) + M + N`. But exec's two sorts hand their last merge pass straight to the join, so the join's reads of the sorted inputs *are* that last pass, already inside `sort()`. Adding `+ M + N` counts them twice. So we use `sort(M) + sort(N)` (decided 2026-11-09). The same holds for intersect and except.

**Hash join: we kept the formula, and exec measures more.** Exec does extra I/O in two cases:
- **Partly full pages.** Exec splits each input into B − 1 partitions, and each partition's last page is only partly full. With many partitions and small inputs this dominates: M = 20, N = 10 at B = 256 measures 1,052, against 90 from the formula (`REPORT_M2.md` §1).
- **Overflow.** A partition too big for memory is joined by nested loop. Example: Emp ⋈ Emp on did at B = 4 is estimated at 282; exec does 524 reads and 94 writes.

Exec cannot change, because both behaviours are §M2.3 rules. We kept 3(M + N) in opt (decided 2026-11-13). How much overflow happens depends on how the keys hash, which statistics cannot show, and changing the formula would change every hash number in our tests. `REPORT_M4.md` §5 shows the measured gaps.

### Two views of cost: the DP and explain

Exec splits the B pages between operators that run at the same time. Opt shows cost in two ways:

- **`dp-table` (the join-order search)** gives **every join all B pages** and uses the plain formulas. That is how §M4.5 computes its table, and the checklist requires it to reproduce.
- **`explain`** gives each node **the pages exec really gives it**: opt calls exec's own frame-splitting function (`divideFrames`, DECISIONS 2026-11-01). Each line shows that node's own cost (§M4.6). Example: a 12-page sort at B = 4 is estimated at 60, which is what exec measures. The plain formula would say 36.

**The two views can pick different winners.** On the §M4.5 shape built as real tables (B = 32), the DP chose (B′ ⋈ C) ⋈ A at an estimated 786 pages. Exec needed **4,324**; the order the DP rejected needed **1,823**.
- **Why:** the lower join runs inside the upper one, and exec gives it only a 1-page block, so it rereads C once per page of B′ (10 times). The DP assumed a 30-page block: one pass.
- **Explain saw it:** with exec's real frames it estimated 4,010 for the lower join, and exec measured 4,021 (4,010 + B′'s 11 pages).
- **We kept the spec's DP** (decided 2026-11-13). It reproduces §M4.5 and changes no finished module. A DP that knows how exec splits the pages is the known improvement. The full numbers are in `REPORT_M4.md` §6.

### What the model does not count

- **Directory pages:** one per 681 data pages, read once per scan. §M4.1 counts data pages only.
- **Hash partitions** that are partly full or overflow (above).
- **A long run of equal keys in sort-merge join**, which exec spills to disk and rereads.

---

## 5. Enumeration strategy: choosing the join order

### The problem

When a query joins several tables, the order of the joins can change the cost enormously, while the answer stays the same. Take the §M4.5 example at B = 32:

| Table | Pages | Rows |
|---|---|---|
| A | 100 | 5,000 |
| B′ | 10 | 500 |
| C | 400 | 20,000 |

The links are `A.x = B′.x` and `B′.y = C.y`. A and C are not linked to each other.

Joining B′ with A first, then adding C, costs 530 pages. Joining B′ with C first, then adding A, costs 650. Joining A with C first has no condition at all: it is a cross product of 5,000 × 20,000 = 100 million rows.

The user's written order is just the order they typed, and it may well be the bad one. So we search for the cheapest order. Trying every order one by one does not scale: 6 tables already have 720 orders, each with a choice of three join algorithms per join. Instead we build the answer up from small pieces and never solve the same piece twice. This is the classic **dynamic programming** search of System R (§M4.5).

### Step 1: find the tables that are joined together

After the rewrite rules, we look at the plan for a group of joins sitting together: a **join block**. We collect:
- the tables (or subqueries) being joined, in the order they were written: here A, B′, C;
- the join conditions that link them: here `A.x = B′.x` and `B′.y = C.y`.

A subquery in FROM gets its own block. It is planned first, and then it is treated like a single table by the block that uses it.

### Step 2: price each table alone

For each table we note the cost of reading it (its scan, with any filters already pushed onto it) and its estimated size:

| Group | Best plan | Cost |
|---|---|---|
| {A} | scan A | 100 |
| {B′} | scan B′ | 10 |
| {C} | scan C | 400 |

### Step 3: build every pair, keeping only the cheapest way

For a pair of tables we try both directions: each table can be the one added second (the inner input of the join). For each direction we try every join algorithm that is allowed. We keep only the cheapest.

**{A, B′}:**
- B′ first, A added second, nested loop: B′'s 10 pages fit in one 30-page block, so A is read once: 10 + 1 × 100 = **110**.
- A first, B′ added second, nested loop: A's 100 pages need ceil(100 / 30) = 4 blocks, so B′ is read 4 times: 100 + 4 × 10 = **140**.
- Sort-merge and hash cost more here (310 and 330).
- **Kept: 110**, B′ first. The group's size (1,000 rows, 20 pages) comes from the estimator and is the same whichever order is used.

**{B′, C}:** in the same way, B′ first with a nested loop over C: 10 + 1 × 400 = **410**.

**{A, C}: skipped.** No condition links A and C, so joining them would be a cross product. We never try a pair that has no linking condition. The one exception: if some table in the query has no condition linking it to the others at all, a cross product is the only way to reach it, and then we allow one.

### Step 4: build bigger groups from the best smaller ones

A group of three is the best plan for two of its tables plus the third table added last. We do not rebuild the pair: we reuse its kept plan and cost from step 3. This reuse is what makes the search fast.

**The whole search at a glance:**

```
 size 1          size 2                                  size 3
 ──────          ──────                                  ──────
 {A}   100 ──┐
             ├──► {A,B′}  110   (B′ first, A read once)  ──┐
 {B′}   10 ──┤                                             ├──► {A,B′,C} + C last:  110 + 20 + 1×400 = 530  ✔ kept
             ├──► {B′,C}  410   (B′ first, C read once)  ──┤
 {C}   400 ──┘                                             └──► {A,B′,C} + A last:  410 + 40 + 2×100 = 650  ✘
                  {A,C}   ✘ skipped: no link between A and C
                                                         (a third way, {A,C} + B′, never exists: {A,C} was skipped)

 Each bigger group reuses the kept best plan of a smaller group. {A,B′} is solved once, never again.
 Answer: B′ first, then A, then C.
```

With n tables there are 2ⁿ groups (8 for three tables, 64 for six). Each group tries at most n "last table" choices × 3 algorithms, so even six tables need at most a few hundred cost calculations.

### Rules we follow while searching

- **Which algorithms are allowed.** A nested loop works for any condition. Sort-merge and hash need a condition of the form `column = column` between the two sides, with the same type on both sides. If the condition also has other parts (`e.did = d.did AND e.age > d.budget`), we join on the equality and apply the rest as a filter right above the join. So the plan we price is exactly the plan that runs.
- **Ties.** When two candidates cost the same, the first one tried wins: the lower-numbered table first, then nested loop before sort-merge before hash. So the same query, data and B always give the same plan.
- **More than six tables.** The spec limits the search to six. With more, we keep the written order, and `dp-table` prints a line saying so. The query still gets a valid plan.

### Step 5: turn the winner into a real plan

1. **Rebuild the tree** from the kept steps, bottom up: (B′ ⋈ A) ⋈ C.
2. **Restore the column order.** Joining in a new order changes the order of the output columns (B′'s columns now come first). If the user wrote `SELECT *`, they expect the written order, so we add one projection on top that puts the columns back.
3. **Rerun the column-trimming rule** (rule 6). The joins moved, so each join's inputs need different columns now.
4. **Choose each join's algorithm one final time,** from the final tree's estimated sizes. This is where B makes a difference: Emp ⋈ Works on eid becomes a hash join at B = 4 and a nested loop at B = 32 and 256 (`REPORT_M4.md` §7).

`opt dp-table` prints the search exactly as above: one row per group, with its best plan and cost, a `—` for skipped groups, and at the end the cheapest alternative for each other table that could have been added last. `REPORT_M4.md` §8 shows it for a four-table query.

### Why we only build "left-deep" plans, and what that costs

Every plan we build adds **one table at a time**: `((A ⋈ B) ⋈ C) ⋈ D`. The input added second to each join is always a plain table, never another join. This shape is called **left-deep**. The alternative, a **bushy** plan, may join two finished joins: `(A ⋈ B) ⋈ (C ⋈ D)`.

```
 left-deep (what we build)                      bushy (what we never build)

            ⋈  → 10 rows                                    ⋈  → 10 rows
           / \                                            /    \
   100,000 ⋈   D                              10 rows  ⋈        ⋈  10 rows
   rows   / \                                         / \      / \
   10 → ⋈    C   ← C added on its own                A   B    C   D
        / \         (100,000 rows!)
       A   B

 Every join adds one plain table.               Two finished joins may meet.
 ✔ small search (720 orders for 6 tables)       ✔ both halves stay at 10 rows
 ✔ rows stream up, nothing written to disk      ✘ 30,240 shapes for 6 tables
 ✔ the reread input is always a plain table     ✘ an inner join would have to be rerun or stored
 ✘ must build the 100,000-row middle step
```

The numbers in the trees: A ⋈ B and C ⋈ D each give 10 rows, and B and C are linked by `B.z = C.z`, where C has 100,000 rows and only 10 different values of z.

---

## 6. Decisions, deviations and flagged spec numbers

This section lists, in one place, every spot where we made a choice the spec leaves open, where we do something different from the spec, or where we think a spec number is wrong. Each item is short. The longer reasoning is in the sections named.

### 6.1 How we write the physical plan for exec

The plan must run in exec with no editing (§M4.6). To make that work:

- **Names of operators.** `scan` becomes `seqscan`, `select` becomes `filter`, and each join becomes `nested_loop_join`, `sort_merge_join` or `hash_join`.
- **Cross product.** Written as `nested_loop_join` with the condition `1 = 1`, because exec needs a condition on every join.
- **Table aliases** (`Emp e`). Kept as a `rename` node, which exec supports.
- **distinct and group_by.** We pick `sort` or `hash` for each one. Hash is chosen only if it is strictly cheaper. It is costed with B − 1 pages, because one page is busy reading the input.
- **Aggregate names.** `COUNT(*) AS n` becomes exec's own name, `count_star` (and `AVG(e.salary)` becomes `avg_e_salary`), because exec's plan format has no place for an alias. So result headers show these names.
- **Estimates.** Kept outside the plan JSON, in the same node order as `exec trace`, so line i of `explain` matches row i of the trace.

### 6.2 Spec numbers we believe are wrong

| Where | Spec says | We get | Why |
|---|---|---|---|
| §M4.5 DP table, last row | 410 + 40 + 100 = **550** | **650** | 40 pages need ceil(40 / 30) = **2** passes over A, not 1: 410 + 40 + 2 × 100 = 650. The winner (530) does not change. |
| §M4.6 explain example | `hash_join est_cost=330` | (layout only) | No sizes in the example give 330. The inputs are about 20 and 10 pages: 3 × (20 + 10) = 90. We copy only the layout of that example. |
| §M4.5 joined sizes | 1,000 rows = **20 pages** | **40 pages** with real widths | 20 pages fits the width of one table's row. A joined row holds both tables' columns, so it is about twice as wide. Our test uses narrower tables so that the spec's 20 still comes out. |
| §M4.2 `age > 60` | 10/50 = 0.20 | 0.20 (same) | Ages 20–70 are 51 whole numbers, and 10 of them are above 60, so the exact answer is 10/51. We keep the spec's formula. |

One more number fix that is not in the spec: rows are rounded up with a tiny safety margin. In floating point, 0.1 × 3 × 10 = 3.0000000000000004, and a plain round-up would turn 3 rows into 4.

### 6.3 Where we do something different from the spec

Each item: what the spec says → what we do → why.

1. **ANALYZE reads the table once** (§M4.1).
   We do this when B is big enough: B ≥ 2 × the number of columns + 1 (at B = 32, up to 15 columns). With a smaller B, we read the table once per column.
   *Why:* each column needs its own sort, and each sort needs 2 pages of memory. The statistics come out the same either way.

2. **Sort-merge join costs `sort(M) + sort(N) + M + N`** (§M4.4).
   We use `sort(M) + sort(N)`. Intersect and except use the same formula.
   *Why:* exec passes the sorts' last pass straight into the merge, so the `+ M + N` would count those reads twice (section 4).

3. **Hash join costs `3 × (M + N)`** (§M4.4).
   We keep this formula, but exec often measures more.
   *Why:* the extra I/O comes from half-empty partition pages and partitions too big for memory. Both depend on the data, which statistics cannot predict (section 4).

4. **One B for everything** (§M4.5).
   The join-order search gives every join all B pages. `explain` gives each node the pages exec really gives it.
   *Why:* the search must reproduce the §M4.5 table (a checklist item), and `explain` must match exec. The two can disagree: section 4 shows one plan that ran 2.4 times slower.

5. **Directory pages.**
   We don't count them; exec does (one extra page per scan).
   *Why:* §M4.1 says `n_pages` counts data pages only.

6. **The unoptimized plan** (§M4.7 #2: "joins in written order, one fixed algorithm, no pushdown").
   We turn off the rules that move filters and columns down (2, 4, 5, 6), skip the join-order search, and use a nested loop for every join.
   *Why:* each phrase of the spec maps to one switch. Nested loop is the only join that works for any condition.

7. **Extra command-line flags.**
   - `--disable-rule N` and `--unoptimized`, needed for the §M4.7 measurements.
   - `--show-rewrite`, to print the trees before and after the rewrites.
   - `--json`, `--frames`, and `--catalog` (the handout's name for `--db`).

8. **Rewrite trees in `explain` show rows only, no cost.**
   *Why:* before the physical plan is built, a join has no algorithm yet, so it has no cost. The physical plan shows both numbers on every line (§M4.6).

### 6.4 Known limits

| Limit | What it causes | Where to read more |
|---|---|---|
| AND assumes the two conditions are unrelated | Correlated columns are underestimated (82 rows instead of 1,618). | `REPORT_M4.md` §4 |
| The search gives every join all B pages | It can pick the slower join order (4,324 vs 1,823 page I/O). | section 4, `REPORT_M4.md` §6 |
| A filtered inner input of a nested loop is costed at its filtered size | Exec rereads the whole table on every pass, so the plan costs more than predicted (q13: 521 vs 307). | `REPORT_M4.md` §3, §5 |
| Page estimates ignore the 4 bytes each row needs in a page's slot list | Narrow rows can take twice the estimated pages. | `REPORT_M4.md` §5 |
| Filters never move below a projection | Rule 5 cannot fire on a subquery's WHERE. | section 3 |
| The search's costs use the filtered size of each input | They rank the orders correctly but are not the real I/O; `explain`'s total is. | `REPORT_M4.md` §5, §8 |
| Hash join extra I/O is not modelled | Half-empty partition pages and overflow cost more than 3 × (M + N). | section 4 |

---

## 7. Readings

1. **Andy Pavlo, CMU 15-445/645 *Intro to Database Systems*, Fall 2024, #15 Query Planning & Optimization.** The formulas and rules we implement come from the spec; this lecture gave us the mental model of an optimizer before we built one. It splits the problem in two: rewrites that are always safe come first, then a cost-based search chooses among the plans that are left. That is the order of our pipeline. It also showed that every estimate rests on assumptions (uniform values, independent columns), and that the useful question is where those assumptions break. That question is why we built the correlated Cust.city / Cust.zip pair to test independence on purpose (`REPORT_M4.md` §4).
2. **P. G. Selinger et al., *Access Path Selection in a Relational Database Management System*, SIGMOD 1979.** This paper is the source of our join-order search (section 5): dynamic programming over left-deep plans, building each group from its best smaller groups, and avoiding cross products when a linked pairing exists. Our defaults for a column with no statistics also come from it: 1/10 for equality and 1/3 for a range (section 2).
