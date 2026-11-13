# REPORT_M4 — Query optimizer measurements

This report measures how good our optimizer's numbers are, and where they are wrong. All numbers come from one run of `python bench/report_m4.py` (from the repo root, after building). The script only calls the command-line tools (`storage`, `opt`, `sql`, `exec`), as a grader would. Its tables are also written to `bench/out/m4/tables.md`. A full run takes about 30 minutes, almost all of it in the unoptimized and rule-off plans, which run nested loops of up to 3 × 10⁸ comparisons.

**The main findings, in short:**
- **Row estimates are mostly exact.** Across the 156 plan nodes of the twenty queries, the median q-error is 1.00. Leaving out 5 nodes under a `limit`, whose row counts say nothing about the estimate, the worst case is 19.7. That case is the correlated query.
- **Optimization pays a lot on joins of three or more tables:** up to 47 times less I/O than the unoptimized plan. On single-table queries it pays nothing.
- **Pushing selections down (rules 1 and 2) is almost all of the gain.** The other five rules save no I/O on this data at B = 32. At B = 4, rule 6 saves I/O on three queries and *costs* I/O on one.
- **Where it breaks:** two correlated columns make the optimizer underestimate a filter by 19.7 times. It keeps a nested loop that costs **1.85 times** the I/O of the plan it picks once the estimate is right (11,064 against 5,966 page I/O at B = 4).
- **Predicted cost = measured I/O, exactly**, for 18 of 20 queries at B = 32 (after the directory pages, which we do not count on purpose). Every remaining gap has a named cause (section 5).
- **The join-order search itself can be wrong** even when every row estimate is exact: on the §M4.5 shape it picks a plan that exec runs 2.4 times slower (section 6).

Words used below:
- **q-error:** how many times an estimate is off, `max(est, act) / min(est, act)`. 1 means exact; 10 means ten times too high or too low.
- **Selectivity:** the fraction of rows a condition keeps.
- **ndv:** the number of distinct values in a column.
- **Page I/O:** pages read plus pages written, exec's counters. This is the cost the optimizer predicts (§M4.4).
- **BNL:** block nested loop join; **SMJ:** sort-merge join; **Hash:** grace hash join.

---

## 0. Setup and assumptions

**Machine and build.** Windows 11, MinGW GCC 15.2 (CLion's bundled toolchain), CMake + Ninja. The counters do not depend on the machine: the same data, plan and B give the same counters on every run (§0.2). Two full runs of the script gave identical tables.

**Assumptions: data and workload.**  Everything below is our own and final data created for Report.

**Data.** We generate all data ourselves with a fixed random seed (Python's `random.Random(3005)`), so every run produces exactly the same tables. The code is `make_tables()` in `bench/report_m4.py`. Each table is loaded with `storage create-table` / `storage load`, then analyzed with `opt analyze` (20 buckets, B = 32).

**The tables at a glance:**

| Table | Rows | Pages | Used for |
|---|---|---|---|
| Dept | 50 | 1 | the twenty queries |
| Emp | 10,000 | 74 | the twenty queries |
| Proj | 500 | 4 | the twenty queries |
| Works | 30,000 | 118 | the twenty queries |
| Cust | 32,000 | 745 | the correlated columns (q05, section 4) |
| Orders | 96,000 | 542 | the correlated join (section 4), q10 |
| A, Bp, C | 5,000 / 500 / 20,000 | 100 / 10 / 400 | the §M4.5 example as real tables (section 6) |

"Pages" are data pages, as the catalog counts them.

**What is in each table.** "Uniform a..b" means each row gets a random whole number between a and b, each value equally likely. The examples are real rows from the generated data.

**Dept** (one row per department)

| Column | Type | Values | Example |
|---|---|---|---|
| did | INT | 0, 1, …, 49 (one per row) | 7 |
| dname | VARCHAR(12) | `dept` + did | `dept07` |
| budget | INT | uniform 100..5000 | 313 |

**Emp** (one row per employee)

| Column | Type | Values | Example |
|---|---|---|---|
| eid | INT | 0, 1, …, 9999 (one per row) | 42 |
| did | INT | uniform 0..49 (the employee's department) | 47 |
| age | INT | uniform 20..69 | 38 |
| salary | INT | uniform 30000..129999 | 65227 |
| name | VARCHAR(16) | `emp` + eid | `emp00042` |

**Proj** (one row per project)

| Column | Type | Values | Example |
|---|---|---|---|
| pid | INT | 0, 1, …, 499 (one per row) | 12 |
| did | INT | uniform 0..49 (the project's department) | 49 |
| pname | VARCHAR(12) | `proj` + pid | `proj012` |
| budget | INT | uniform 1000..99999 | 2258 |

**Works** (who works on which project; 3 rows per employee)

| Column | Type | Values | Example |
|---|---|---|---|
| eid | INT | 0, 0, 0, 1, 1, 1, … (each employee 3 times) | 42 |
| pid | INT | uniform 0..499 | 299 |
| hours | INT | uniform 1..40 | 4 |

**Cust** (customers; **the two correlated columns are here**)

| Column | Type | Values | Example |
|---|---|---|---|
| cid | INT | 0, 1, …, 31999 (one per row) | 19 |
| zip | INT | uniform 0..999 | 10 |
| city | VARCHAR(8) | **decided by the zip**: zips 0–49 → `City00`, 50–99 → `City01`, …, 950–999 → `City19` | `City00` |
| name | VARCHAR(16) | `cust` + cid | `cust00019` |
| note | VARCHAR(60) | 60 × `n`, only to make each row wide (43 rows per page) | `nnn…n` |

**Orders** (3 orders per customer on average)

| Column | Type | Values | Example |
|---|---|---|---|
| oid | INT | 0, 1, …, 95999 (one per row) | 100 |
| cid | INT | uniform 0..31999 (the customer) | 23340 |
| amount | INT | uniform 1..1000 | 423 |
| status | VARCHAR(8) | one of `open`, `paid`, `shipped` | `shipped` |

**A, Bp, C** (the §M4.5 example built as real tables; no random numbers)

| Table | Columns | Values | Pages |
|---|---|---|---|
| A | x INT, pad VARCHAR(71) | x = row number mod 2,500 (each value twice) | 100 |
| Bp | x INT, y INT, pad VARCHAR(67) | x = y = row number (0..499) | 10 |
| C | y INT, pad VARCHAR(71) | y = row number mod 5,000 (each value 4 times) | 400 |

The `pad` column is filler, so that exactly 50 rows fit on a page and the page counts match §M4.5. `Bp` is the spec's table B′ ("B prime"); a table name cannot contain the `′` character.

**Why Cust and Orders are big.** The correlation in Cust only changes the chosen plan when the misjudged input is larger than a nested-loop block. So Cust needs wide rows and many of them (section 4).

**The twenty queries** (`QUERIES` in the script). They are compiled with `sql compile`. Joins of three or more tables are written with JOIN ON: written with commas, the unoptimized plan (no pushdown) would build cross products of millions of rows. q06 and q09 use commas.

| Query | Kind | SQL |
|---|---|---|
| q01 | equality | `SELECT * FROM Emp e WHERE e.did = 7` |
| q02 | range | `SELECT e.name, e.salary FROM Emp e WHERE e.age > 60` |
| q03 | AND of equality and range | `SELECT e.eid FROM Emp e WHERE e.did = 7 AND e.age > 60` |
| q04 | OR | `SELECT e.eid, e.age FROM Emp e WHERE e.age < 25 OR e.salary > 120000` |
| q05 | AND on the correlated pair | `SELECT c.cid, c.name FROM Cust c WHERE c.city = 'City00' AND c.zip < 50` |
| q06 | 2-way, commas | `SELECT e.name, d.dname FROM Emp e, Dept d WHERE e.did = d.did AND d.budget > 4000` |
| q07 | 2-way, JOIN ON | `SELECT e.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE w.hours > 35` |
| q08 | 2-way, JOIN ON | `SELECT p.pname, d.dname FROM Proj p JOIN Dept d ON p.did = d.did WHERE p.budget < 10000` |
| q09 | 2-way, commas | `SELECT w.eid, p.pname FROM Works w, Proj p WHERE w.pid = p.pid AND p.budget > 90000` |
| q10 | AND on one table | `SELECT o.oid, o.amount FROM Orders o WHERE o.amount > 990 AND o.status = 'paid'` |
| q11 | 3-way | `SELECT e.name, p.pname FROM Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid WHERE p.budget > 95000` |
| q12 | 3-way | `SELECT e.name, w.hours FROM Dept d JOIN Emp e ON d.did = e.did JOIN Works w ON e.eid = w.eid WHERE d.dname = 'dept07' AND w.hours < 5` |
| q13 | 4-way | `SELECT e.name, p.pname, d.dname FROM Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid JOIN Dept d ON p.did = d.did WHERE d.budget < 500 AND e.age > 65` |
| q14 | group_by + HAVING | `SELECT e.did, COUNT(*) AS n, AVG(e.salary) AS avg_sal FROM Emp e GROUP BY e.did HAVING COUNT(*) > 210` |
| q15 | join + group_by | `SELECT d.dname, COUNT(*) AS n FROM Emp e JOIN Dept d ON e.did = d.did WHERE e.age > 50 GROUP BY d.dname` |
| q16 | sort + limit | `SELECT e.name, e.salary FROM Emp e WHERE e.did < 5 ORDER BY e.salary DESC LIMIT 10` |
| q17 | distinct | `SELECT DISTINCT p.did FROM Proj p WHERE p.budget > 50000` |
| q18 | union | `SELECT e.eid FROM Emp e WHERE e.age > 65 UNION SELECT w.eid FROM Works w WHERE w.hours = 40` |
| q19 | except | `SELECT e.eid FROM Emp e WHERE e.did = 3 EXCEPT SELECT w.eid FROM Works w WHERE w.hours > 20` |
| q20 | derived table | `SELECT t.did, t.n FROM (SELECT e.did, COUNT(*) AS n FROM Emp e GROUP BY e.did) AS t WHERE t.did < 10` |

**B.** Measurements 1–3 use **B = 32**, the spec's example budget. Measurements 2, 3 and 5 are repeated at **B = 4**, the smallest B the spec allows; section 7 also uses B = 256.

**How estimates are paired with actual rows.** `opt explain --json` gives each node's `est_rows` in **pre-order** (root, left subtree, right subtree). `exec trace --json` on the plan from `opt plan` gives each node's actual `tuples_out` in the same order. Row i of one is node i of the other. The script checks that both lists have the same length and stops if not.

**Two counting rules (ours):**
- **An actual count of 0 counts as 1.** Our estimates are never below 1 row (§M4.2), so 1 is the smallest number they can be compared with.
- **The inner input of a nested loop is counted per pass.** Exec reruns the inner input once per block of the outer and counts its rows every time, so a 500-row inner read 3 times reports 1,500. The estimate is for one pass (500). So each node of the inner subtree is divided by the number of passes, read off its scan: rows returned / table rows (1,500 / 500 = 3).

**Self-checks.** The script stops with a message if:
1. the estimates and the trace rows do not pair up one to one, or an estimate is below 1 row;
2. the optimized and the unoptimized plan return different row counts (for every query at B = 32 and 4, and for every rule turned off);
3. the correlated query's estimate is not at least 10 times below the actual;
4. no query changes its join algorithm across B = 4, 32, 256.

Each check was shown to work: we corrupted its input (one estimate dropped, an estimate of 0.5, one extra unoptimized row, the correlated estimate made 10 times larger, all algorithms forced to "BNL"), and the script stopped with the matching message.

---

## 1. Estimation quality

**Takeaway: the estimates are almost always right.** For all 156 plan nodes of the twenty queries (B = 32):
- **median q-error = 1.00**;
- **worst = 100.30**, but that is a sort under a `limit` (not a real error, see below);
- leaving out the 5 nodes under a limit: median 1.00, **worst 19.73**, the correlated query.

**How far off the estimates are** (number of nodes per q-error range):

```
exactly 1.00   ████████████████████████████████████████ 105
1.00 – 2       ██████████████████ 47
2 – 10         █ 1    q19 except (8.91)
over 10        █ 3    q05 project and filter (19.73 each); q16 sort under a limit (100.30)
```

**The worst node of each query** (q-error, node, estimated vs actual rows; every node is in the appendix):

```
q01    1.01  filter    est    205  actual    207
q02    1.12  project   est  1,969  actual  1,761
q03    1.28  project   est     41  actual     32
q04    1.09  project   est  2,016  actual  1,849
q05   19.73  project   est     82  actual  1,618
q06    1.02  project   est  2,000  actual  1,965
q07    1.00  project   est  3,749  actual  3,749
q08    1.09  project   est     38  actual     35
q09    1.04  project   est     47  actual     45
q10    1.03  project   est    327  actual    336
q11    1.50  project   est     21  actual     14
q12    1.03  project   est    200  actual    207
q13    1.25  project   est    993  actual    796
q14    1.70  filter    est     17  actual     10
q15    1.00  group_by  est     50  actual     50
q16  100.30  sort      est  1,003  actual     10
q17    1.00  distinct  est     50  actual     50
q18    1.25  project   est    993  actual    796
q19    8.91  except    est    196  actual     22
q20    1.10  rename    est     11  actual     10
```

**Where the big errors come from:**

| Query | q-error | Cause |
|---|---|---|
| q16 | 100.3 | **Not a real error.** `limit 10` stops reading after 10 rows, so the sort below it reports 10 rows, not its 1,003. |
| q05 | 19.7 | **Correlated columns:** `city = 'City00' AND zip < 50` is estimated at 82 rows, actual 1,618 (section 4). |
| q19 | 8.9 | **`except` is assumed to remove nothing:** estimated 196 (the left side), but 188 of department 3's 210 employees are removed, leaving 22. |
| q14 | 1.7 | **HAVING on an aggregate has no statistics:** the default 1/3 is used, so 50 / 3 = 17 estimated, actual 10. |
| others | 1.0–1.5 | Small histogram and independence errors (`age > 60`: 1,969 vs 1,761), and a small filter error carried up through joins (q11: 21 vs 14). |

Most nodes are exact because scans take their row counts from the catalog, histograms are accurate to a few percent on uniform columns, and our joins are on keys, where the §M4.2 join formula is right.

---

## 2. Does optimization pay

**Takeaway: on joins of three or four tables, our plan needs up to 47 times less I/O than the unoptimized plan. On single-table queries there is nothing to choose, so the two plans cost the same.**

- **Ratio range:** 1.00 to 47.12 at B = 32; 1.00 to 36.44 at B = 4.
- **Unoptimized plan** (§M4.7 #2): joins in written order, a nested loop for every join, and no pushdown (rules 2, 4, 5, 6 off).
- **Ratio** = unoptimized I/O ÷ optimized I/O, so 2× means our plan halves the I/O. Both plans always return the same rows.

**The queries where optimization pays** (bar = ratio; in brackets: optimized vs unoptimized page I/O):

```
q13  4-way           B = 32  ██████████████████████████████  47.12×   (201 vs 9,472)
                     B = 4   ███████████████                 23.91×   (521 vs 12,458)
q11  3-way           B = 32  █████████████████████████████   44.92×   (199 vs 8,939)
                     B = 4   ███████████████████████         36.44×   (282 vs 10,276)
q12  3-way           B = 32  ██                               3.43×   (196 vs 672)
                     B = 4   ███████████████████████         35.80×   (198 vs 7,088)
q07  2-way, JOIN ON  B = 32  █                                2.23×   (194 vs 432)
                     B = 4   ████████                        12.69×   (353 vs 4,478)
q09  2-way, commas   B = 32  █                                1.12×   (124 vs 139)
                     B = 4   ██                               3.34×   (124 vs 414)
q06  2-way, commas   B = 32  █                                1.04×   (77 vs 80)
                     B = 4   █                                1.94×   (77 vs 149)
q08  2-way, JOIN ON  B = 32  █                                1.00×   (7 vs 7)
                     B = 4   █                                1.29×   (7 vs 9)
q15  join + group_by B = 32  █                                1.00×   (77 vs 77)
                     B = 4   ██                               2.88×   (77 vs 222)
```

**Ratio 1.00 at both B** (same I/O in both plans; page I/O in brackets, B = 32 / B = 4 where they differ): q01 (75), q02 (75), q03 (75), q04 (75), q05 (747), q10 (543), q14 (75), q16 (75 / 82), q17 (5), q18 (202), q19 (255 / 375), q20 (75).

**Why the ratio varies so much:**
- **One table (ratio 1):** a filter, sort or group_by over one scan reads the table once whatever the plan.
- **Two tables (1–3× at B = 32):** both inputs fit in the 30-page nested-loop block, so each table is read once in either plan. At B = 4 (a 2-page block) the unoptimized plan's large, unfiltered outer needs many passes (q07: 12.69×).
- **Three or four tables (3–47×):** the written order is bad. In q11 the unoptimized plan first joins Emp and Works (30,000 rows) and only then applies `p.budget > 95000`. Our plan starts from the 21 Proj rows that pass the filter, so every later join is small: 199 vs 8,939 page I/O.

---

## 3. Which rule mattered

**Takeaway: rules 1 and 2 (split AND, push filters below joins) give almost all of the gain. Rules 3, 4, 5 and 7 save no I/O on this data. Rule 6 matters only at B = 4, where it helps three queries and hurts one.**

Each line turns one rule off (`--disable-rule N`) and reruns all twenty queries. The bar is the **extra** page I/O without that rule; "total" is the whole workload's I/O without it. All queries still return the same rows.

**B = 32** (all rules on: 3,352 page I/O)

```
rule 2 off   ██████████████████████████████  +14,118   total 17,470
rule 1 off   ████████████████                 +7,348   total 10,700
rule 3 off   ·                                    +0   total  3,352
rule 4 off   ·                                    +0   total  3,352
rule 5 off   ·                                    +0   total  3,352
rule 6 off   ·                                    +0   total  3,352
rule 7 off   ·                                    +0   total  3,352
```

Queries whose I/O changed (with the rule → without it):

- rule 2 off: q07 (194 → 344), q11 (199 → 6,819), q12 (196 → 434), q13 (201 → 7,311)
- rule 1 off: q12 (196 → 434), q13 (201 → 7,311)

**B = 4** (all rules on: 4,043 page I/O)

```
rule 2 off   ██████████████████████████████  +12,462   total 16,505
rule 1 off   ███████████████████████          +9,652   total 13,695
rule 6 off   █                                  +385   total  4,428
rule 3 off   ·                                    +0   total  4,043
rule 4 off   ·                                    +0   total  4,043
rule 5 off   ·                                    +0   total  4,043
rule 7 off   ·                                    +0   total  4,043
```

Queries whose I/O changed (with the rule → without it):

- rule 2 off: q07 (353 → 1,092), q09 (124 → 243), q11 (282 → 2,353), q12 (198 → 3,261), q13 (521 → 6,991)
- rule 1 off: q09 (124 → 243), q12 (198 → 3,261), q13 (521 → 6,991)
- rule 6 off: q07 (353 → 526), q11 (282 → 585), q12 (198 → 321), q13 (521 → 307)

**Why each rule is worth what it is:**

| Rule | Worth | Why |
|---|---|---|
| 2 (push filter below join) | the most | Without it, filters stay above the joins and every join works on whole tables (q11: 199 → 6,819). |
| 1 (split AND) | a lot, on 2–3 queries | Rule 2 can only move a condition on one table, so `d.dname = 'dept07' AND w.hours < 5` must be split first (q12, q13). Queries with a single WHERE condition don't need it. |
| 3 (cross + `=` → join) | 0 I/O | A nested loop reads the same pages whatever its condition. It saves CPU: q09 makes 1,350,500 comparisons with it, 2,700,500 without. |
| 4 (through rename/union) | 0 | Fires only in q20; the scan below reads all of Emp either way. |
| 5 (below group_by) | 0 | **Never fires:** sql puts a projection above every group_by, and our rewriter does not move filters below a projection (`DESIGN_M4.md` §3). |
| 6 (trim columns at joins) | 0 at B = 32; +385 at B = 4 | At B = 32 every input already fits in one block. At B = 4 narrower rows mean fewer blocks (q07, q11, q12 improve). **But q13 is worse with it (521 with, 307 without):** trimmed, filtered Emp looks like 4 pages, so the search picks a nested loop with Emp as the inner. But exec rereads all 75 pages of Emp on each of 5 passes (section 5). |
| 7 (remove no-op projection) | 0 | Cosmetic, as the spec says. |

---

## 4. Where it breaks: two correlated columns

**Takeaway: the optimizer guesses 82 rows where there are 1,618 (20 times fewer), because it assumes two conditions are unrelated. With that wrong guess it keeps a nested loop that costs 1.85 times the I/O of the plan it picks when the guess is right (11,064 vs 5,966 at B = 4).**

```sql
SELECT * FROM Cust c, Orders o
 WHERE c.cid = o.cid AND c.city = 'City00' AND c.zip < 50;
```

**The data:** the zip decides the city, so the two conditions keep the *same* customers.

```
zip:   0 ──── 49 │ 50 ──── 99 │ 100 ─── 149 │ ... │ 950 ─── 999
city:   City00   │   City01   │   City02    │ ... │   City19
        └── zip < 50 and city = 'City00' select exactly this block (5 %)
```

**The estimate goes wrong:**

```
                     guessed                        actual
city = 'City00'      1,618 rows  (5 %)        ✔     1,618
zip  < 50            1,618 rows  (5 %)        ✔     1,618
both (AND)              82 rows  (5 % × 5 %)  ✘     1,618   ← 19.7× too low
                     └── assumes the two conditions are unrelated (independence, §M4.2)
```

**The assumption that failed:** independence of `p AND q`, `sel(p AND q) = sel(p) × sel(q)` (§M4.2). It is right only if knowing p tells you nothing about q. Here the zip tells you the city exactly.

**So the optimizer picks the wrong join (B = 4):**

```
 what it believes                          what really happens
 ────────────────                          ───────────────────
 filtered Cust = 82 rows ≈ 2 pages         filtered Cust = 1,618 rows ≈ 37 pages
          │                                         │
 fits in one 2-page block                  needs 19 blocks
          │                                         │
 nested loop: read Orders 1×               nested loop: read Orders 19×
 cost on paper: 2 + 1 × 542 = 544          real I/O: 747 (Cust) + 19 × 543 (Orders) = 11,064
```

On paper, a hash join would cost 3 × (2 + 542) = 1,632, so the nested loop (544) wins. With the right size (36 pages), the nested loop would need 18 passes (36 + 18 × 542 = 9,792), and the hash join (1,734) wins. That is the plan opt picks when we give it only `zip < 50`, whose guess is exact.

**Bad plan vs the better one.** To get the better plan, we took the plan opt picks for `zip < 50` alone, put both conditions back into its filter, and ran it with `exec`. Both plans return the same 4,796 rows.

```
B     bad plan (nested loop)          better plan                  bad ÷ better
 4    ███████████████████████ 11,064  ████████████ 5,966   (hash)       1.85×
 6    █████████████ 6,177             ███████ 3,561        (hash)       1.73×
 8    █████████ 4,548                 ██████ 2,982         (hash)       1.53×
12    ██████ 2,919                    █████ 2,653          (hash)       1.10×
16    █████ 2,376                     █████ 2,376  (same plan)          1.00×
32    ████ 1,833                      ████ 1,833   (same plan)          1.00×
```

The damage shrinks as B grows: a bigger block holds the 37 real pages in fewer passes, and from B = 16 the nested loop is the right choice anyway. The hash join at B = 4 does more than 3 × (M + N) itself (4,093 reads, 583 writes, because partitions overflow; section 5), and it still wins.

**What would have prevented it:** a statistic on **(city, zip) together**, for example the number of distinct (city, zip) pairs. That number is 1,000, the same as the number of zips, which shows that the zip decides the city. The optimizer would then use the zip's 5 % alone and guess 1,618 rows exactly. PostgreSQL stores this as `CREATE STATISTICS … (dependencies)`. A two-column histogram on (city, zip) would also count the 1,618 rows directly.

---

## 5. Predicted cost against measured I/O

**Takeaway: the predicted I/O equals exec's measured I/O exactly for 18 of 20 queries at B = 32 and 13 of 20 at B = 4 (two more within 1 page). Every larger gap has a known cause.**

How to read it:
- **Predicted** = the sum of `explain`'s per-node costs, minus one read of each operator's inputs. Every §M4.4 formula already includes reading its inputs, and in exec those reads are the inputs' own scans.
- **Measured** = exec's page I/O minus the directory pages. We don't count directory pages on purpose (§M4.1 counts data pages only); it is one page per scan pass.
- ✔ = exact; ≈ = within 1 page (rounding); ← = a real gap, explained below.

```
query joins          B = 32: predicted  measured  gap    B = 4: predicted  measured  gap
q01   no join                       74        74   +0 ✔                 74        74   +0 ✔
q02   no join                       74        74   +0 ✔                 74        74   +0 ✔
q03   no join                       74        74   +0 ✔                 74        74   +0 ✔
q04   no join                       74        74   +0 ✔                 74        74   +0 ✔
q05   no join                      745       745   +0 ✔                745       745   +0 ✔
q06   BNL                           75        75   +0 ✔                 75        75   +0 ✔
q07   BNL                          192       192   +0 ✔                278       351  +73 ←
q08   BNL                            5         5   +0 ✔                  5         5   +0 ✔
q09   BNL                          122       122   +0 ✔                122       122   +0 ✔
q10   no join                      542       542   +0 ✔                542       542   +0 ✔
q11   BNL, BNL                     196       196   +0 ✔                235       278  +43 ←
q12   BNL, BNL                     193       193   +0 ✔                194       195   +1 ≈
q13   BNL, BNL, BNL                197       197   +0 ✔                217       513 +296 ←
q14   no join                       74        74   +0 ✔                 74        74   +0 ✔
q15   BNL                           75        75   +0 ✔                 75        75   +0 ✔
q16   no join                       74        74   +0 ✔                 82        81   -1 ≈
q17   no join                        4         4   +0 ✔                  4         4   +0 ✔
q18   no join                      192       200   +8 ←                192       200   +8 ←
q19   no join                      224       253  +29 ←                254       373 +119 ←
q20   no join                       74        74   +0 ✔                 74        74   +0 ✔
```

**The causes of the gaps:**

```
q18 +8,  q19 +29 / +119  ← sort runs need more pages than rows × width / 4096: each row also needs a 4-byte slot,
                           so 14,965 four-byte rows take 30 pages, not 15
q11 +43, q13 +296 (B = 4) ← a filtered inner input is reread whole: the formula counts 4 filtered pages per pass,
                           exec reads all 75 pages of Emp on each of 5 passes
q07 +73 (B = 4)           ← hash join: half-empty partition pages and partitions too big for memory
correlated query (§4)     ← wrong row estimate: predicted 1,287, measured 11,064
§M4.5 shape (§6)          ← not a gap here: explain already uses exec's real memory split (the search does not)
```

**Hash join: which number is right?** §M4.4 says: "If the modules disagree about a hash join's cost, say which is right and fix the other."
- **Exec's measured I/O is right.** It does the extra work for real: half-empty partition pages, and a nested loop for each partition too big for memory (Emp ⋈ Emp on did at B = 4: estimate 282, exec 524 reads + 94 writes; `REPORT_M2.md` §1: 90 vs 1,052 at B = 256).
- **We kept opt's 3 × (M + N).** Exec can't change: both behaviours are §M2.3 rules. And how much overflow happens depends on how the keys hash, which statistics cannot show.
- **Sort-merge is the opposite case:** there exec showed the formula counted the merge's reads twice, so we fixed the formula to `sort(M) + sort(N)` (`DESIGN_M4.md` §4).

---

## 6. The join-order search picks a plan that runs 2.4 times slower

**Takeaway: on the §M4.5 example built as real tables (B = 32), every row estimate is exact, but the search still picks the worse join order: 4,324 page I/O against 1,823. It assumes every join gets all B pages of memory; in exec, a join running under another join gets a 1-page block.**

The tables: A = 100 pages, B′ (`Bp`) = 10 pages, C = 400 pages (section 0). Query: `SELECT * FROM A, Bp, C WHERE A.x = Bp.x AND Bp.y = C.y`.

**What the search saw** (`opt dp-table`, every join given all 32 pages):

| Subset | Best plan considered | Cost |
|---|---|---|
| {A} | scan A | 100 |
| {Bp} | scan Bp | 10 |
| {C} | scan C | 400 |
| {A,Bp} | Bp outer, BNL with A inner | 110 |
| {A,C} | no predicate, skipped | — |
| {Bp,C} | Bp outer, BNL with C inner | 410 |
| {A,Bp,C} | {Bp,C} outer (76 pages), BNL with A inner | 786 |
| | alternative: {A,Bp} outer (38 pages), BNL with C inner | 948 |

With real row widths, the joined groups are 38 and 76 pages, not the spec's 20 and 40 (`DESIGN_M4.md` §6), so the search prefers B′ ⋈ C first:
- 786 = 410 + 76 + ceil(76 / 30) = 3 passes over A (300);
- 948 = 110 + 38 + ceil(38 / 30) = 2 passes over C (800).

**What exec did** (reads per scan; each scan also reads 1 directory page):

```
 the search's choice: (B′ ⋈ C) ⋈ A            the rejected order: (B′ ⋈ A) ⋈ C
 search cost 786                              search cost 948

 nested loop ⋈ A   (28-page block)            nested loop ⋈ C   (28-page block)
 ├── nested loop ⋈ C  (1-page block!)         ├── nested loop ⋈ A  (1-page block!)
 │   ├── scan B′          11                  │   ├── scan B′          11
 │   └── scan C   10 × 401 = 4,010            │   └── scan A   10 × 101 = 1,010
 └── scan A        3 × 101 =   303            └── scan C    2 × 401 =   802
                             ─────                                        ─────
 measured                    4,324            measured                    1,823
```

**Why:** both joins run at the same time and share the 32 pages. Exec gives the lower join only its minimum, a 1-page block, so it rereads its inner table once per page of B′ (10 times). The search assumed a 30-page block (one pass). The extra rereads hit C (400 pages) in the chosen plan, but only A (100 pages) in the other.

**What was right:** every row estimate was exact (2,000 and 4,000 at the joins). `explain`, which uses exec's real memory split, predicted the problem: 4,010 for the lower join, exactly what exec read from C. Its plan total equals exec's measured I/O minus the 14 directory reads.

**The assumption that failed:** "every join gets the whole memory budget B" (§M4.5). Inside one budget of B pages (§0.2), two joins running together cannot both have it.

**What would fix it** (not done): a search that knows how exec splits memory, or an exec rule that gives a lower join more than its minimum. Neither could reproduce §M4.5's 530, which the checklist requires, so we kept the spec's search (decided 2026-11-13).

---

## 7. Changing B changes the join algorithm

**Takeaway: q07 uses a hash join at B = 4 and a nested loop at B = 32 and 256 (§M4.9 checklist).** The correlated join of section 4 also changes: hash at B = 4 to 12, nested loop from B = 16.

The join algorithms `opt plan` picks for each query with a join (BNL = nested loop):

```
q06   B = 4: BNL            B = 32: BNL            B = 256: BNL           
q07   B = 4: Hash           B = 32: BNL            B = 256: BNL              ← changes
q08   B = 4: BNL            B = 32: BNL            B = 256: BNL           
q09   B = 4: BNL            B = 32: BNL            B = 256: BNL           
q11   B = 4: BNL, BNL       B = 32: BNL, BNL       B = 256: BNL, BNL      
q12   B = 4: BNL, BNL       B = 32: BNL, BNL       B = 256: BNL, BNL      
q13   B = 4: BNL, BNL, BNL  B = 32: BNL, BNL, BNL  B = 256: BNL, BNL, BNL 
q15   B = 4: BNL            B = 32: BNL            B = 256: BNL           
```

**Why q07 changes:** after rule 6, the filtered Works side is estimated at 8 pages and Emp's side at 35.

```
B = 32   nested loop: 1 block   8 + 1 × 35 =  43   ✔ cheapest
         hash join:              3 × (8 + 35) = 129

B = 4    nested loop: 4 blocks  8 + 4 × 35 = 148
         hash join:              3 × (8 + 35) = 129   ✔ cheapest
```

The other joins keep the nested loop at every B, because one input is tiny (Dept: 1 page; filtered Proj: 1 page) and fits in any block.

---

## 8. The DP table for a four-relation query

q13, `Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid JOIN Dept d ON p.did = d.did WHERE d.budget < 500 AND e.age > 65`, at B = 32 (`opt dp-table`, unchanged). One row per subset of the four relations. A subset whose relations are not linked by a join predicate is skipped (`—`), as §M4.5 requires. The last rows show, for the full set, the cheapest plan for each other relation joined last.

Join block: e, w, p, d

| Subset | Best plan considered | Cost |
|---|---|---|
| {e} | scan e | 74 |
| {w} | scan w | 118 |
| {p} | scan p | 4 |
| {d} | scan d | 1 |
| {e,w} | e outer, BNL with w inner | 63 |
| {e,p} | no predicate, skipped | — |
| {w,p} | p outer, BNL with w inner | 62 |
| {e,d} | no predicate, skipped | — |
| {w,d} | no predicate, skipped | — |
| {p,d} | d outer, BNL with p inner | 4 |
| {e,w,p} | {e,w} outer (17 pages), BNL with p inner | 83 |
| {e,w,d} | no predicate, skipped | — |
| {e,p,d} | no predicate, skipped | — |
| {w,p,d} | {p,d} outer (1 pages), BNL with w inner | 64 |
| {e,w,p,d} | {w,p,d} outer (11 pages), BNL with e inner | 79 |
| | alternative: {e,w,p} outer (29 pages), BNL with d inner | 113 |

How to read it: e, w, p, d are the relations in written order. A base relation's cost is its scan (the pushed-down filters cost no I/O of their own). {p,d} = 4: d's filtered rows (1 page) as the outer, one pass over p's 3 projected pages, 1 + 1 × 3 = 4 (§M4.5 convention: the base outer's read is the M of the formula). The winner starts from the two filtered small tables, d then p, adds w (64), and joins Emp last as the inner (79). The written order (e ⋈ w first) costs 113 for its best last step.

**The DP's 79 is not the plan's real I/O.** Under the §M4.5 convention (chunk 01 decision 2), M and N are each input's *estimated output* pages: after the pushed-down filters and projections, Works is 59 pages instead of 118, filtered Emp 4 instead of 74. Exec still reads every table whole: 197 data pages + 4 directory pages = 201 (section 5). The DP's numbers rank the orders; the plan's real cost is explain's per-node total (197, exact). Because every order pays for the same full scans, this does not change the ranking here. It does when a filtered input is the inner of a nested loop (section 3, rule 6 and q13 at B = 4).

---

## Appendix: estimated and actual rows at every node (B = 32)

One block per query, nodes in the same order as `opt explain` (indentation shows the tree). The SQL of each query is in section 0. For the inner input of a nested loop, the actual rows are counted per pass (section 0). `*` marks a node under a `limit`: its actual count is cut short by the limit, so it says nothing about the estimate (section 1).

**q01** (equality)

```
Node                    Est.   Actual  q-error
----------------------------------------------
filter (e.did = 7)       205      207     1.01
  rename e            10,000   10,000     1.00
    seqscan Emp       10,000   10,000     1.00
```

**q02** (range)

```
Node                            Est.   Actual  q-error
------------------------------------------------------
project [e.name, e.salary]     1,969    1,761     1.12
  filter (e.age > 60)          1,969    1,761     1.12
    rename e                  10,000   10,000     1.00
      seqscan Emp             10,000   10,000     1.00
```

**q03** (AND of equality and range)

```
Node                                     Est.   Actual  q-error
---------------------------------------------------------------
project [e.eid]                            41       32     1.28
  filter (e.did = 7 and e.age > 60)        41       32     1.28
    rename e                           10,000   10,000     1.00
      seqscan Emp                      10,000   10,000     1.00
```

**q04** (OR)

```
Node                                            Est.   Actual  q-error
----------------------------------------------------------------------
project [e.eid, e.age]                         2,016    1,849     1.09
  filter (e.age < 25 or e.salary > 120000)     2,016    1,849     1.09
    rename e                                  10,000   10,000     1.00
      seqscan Emp                             10,000   10,000     1.00
```

**q05** (AND on the correlated pair)

```
Node                                             Est.   Actual  q-error
-----------------------------------------------------------------------
project [c.cid, c.name]                            82    1,618    19.73
  filter (c.city = 'City00' and c.zip < 50)        82    1,618    19.73
    rename c                                   32,000   32,000     1.00
      seqscan Cust                             32,000   32,000     1.00
```

**q06** (2-way, commas)

```
Node                                    Est.   Actual  q-error
--------------------------------------------------------------
project [e.name, d.dname]              2,000    1,965     1.02
  nested_loop_join (e.did = d.did)     2,000    1,965     1.02
    project [d.did, d.dname]              10       10     1.00
      filter (d.budget > 4000)            10       10     1.00
        rename d                          50       50     1.00
          seqscan Dept                    50       50     1.00
    project [e.did, e.name]           10,000   10,000     1.00
      rename e                        10,000   10,000     1.00
        seqscan Emp                   10,000   10,000     1.00
```

**q07** (2-way, JOIN ON)

```
Node                                    Est.   Actual  q-error
--------------------------------------------------------------
project [e.name, w.hours]              3,749    3,749     1.00
  nested_loop_join (e.eid = w.eid)     3,749    3,749     1.00
    project [w.eid, w.hours]           3,749    3,749     1.00
      filter (w.hours > 35)            3,749    3,749     1.00
        rename w                      30,000   30,000     1.00
          seqscan Works               30,000   30,000     1.00
    project [e.eid, e.name]           10,000   10,000     1.00
      rename e                        10,000   10,000     1.00
        seqscan Emp                   10,000   10,000     1.00
```

**q08** (2-way, JOIN ON)

```
Node                                    Est.   Actual  q-error
--------------------------------------------------------------
project [p.pname, d.dname]                38       35     1.09
  nested_loop_join (p.did = d.did)        38       35     1.09
    project [d.did, d.dname]              50       50     1.00
      rename d                            50       50     1.00
        seqscan Dept                      50       50     1.00
    project [p.did, p.pname]              38       35     1.09
      filter (p.budget < 10000)           38       35     1.09
        rename p                         500      500     1.00
          seqscan Proj                   500      500     1.00
```

**q09** (2-way, commas)

```
Node                                    Est.   Actual  q-error
--------------------------------------------------------------
project [w.eid, p.pname]               2,820    2,726     1.03
  nested_loop_join (w.pid = p.pid)     2,820    2,726     1.03
    project [p.pid, p.pname]              47       45     1.04
      filter (p.budget > 90000)           47       45     1.04
        rename p                         500      500     1.00
          seqscan Proj                   500      500     1.00
    project [w.eid, w.pid]            30,000   30,000     1.00
      rename w                        30,000   30,000     1.00
        seqscan Works                 30,000   30,000     1.00
```

**q10** (AND on one table)

```
Node                                                 Est.   Actual  q-error
---------------------------------------------------------------------------
project [o.oid, o.amount]                             327      336     1.03
  filter (o.amount > 990 and o.status = 'paid')       327      336     1.03
    rename o                                       96,000   96,000     1.00
      seqscan Orders                               96,000   96,000     1.00
```

**q11** (3-way)

```
Node                                        Est.   Actual  q-error
------------------------------------------------------------------
project [e.name, p.pname]                  1,260      848     1.49
  nested_loop_join (e.eid = w.eid)         1,260      848     1.49
    project [p.pname, w.eid]               1,260      848     1.49
      nested_loop_join (w.pid = p.pid)     1,260      848     1.49
        project [p.pid, p.pname]              21       14     1.50
          filter (p.budget > 95000)           21       14     1.50
            rename p                         500      500     1.00
              seqscan Proj                   500      500     1.00
        project [w.eid, w.pid]            30,000   30,000     1.00
          rename w                        30,000   30,000     1.00
            seqscan Works                 30,000   30,000     1.00
    project [e.eid, e.name]               10,000   10,000     1.00
      rename e                            10,000   10,000     1.00
        seqscan Emp                       10,000   10,000     1.00
```

**q12** (3-way)

```
Node                                        Est.   Actual  q-error
------------------------------------------------------------------
project [e.name, w.hours]                     61       62     1.02
  nested_loop_join (e.eid = w.eid)            61       62     1.02
    project [e.eid, e.name]                  200      207     1.03
      nested_loop_join (d.did = e.did)       200      207     1.03
        project [d.did]                        1        1     1.00
          filter (d.dname = 'dept07')          1        1     1.00
            rename d                          50       50     1.00
              seqscan Dept                    50       50     1.00
        project [e.eid, e.did, e.name]    10,000   10,000     1.00
          rename e                        10,000   10,000     1.00
            seqscan Emp                   10,000   10,000     1.00
    project [w.eid, w.hours]               3,044    3,044     1.00
      filter (w.hours < 5)                 3,044    3,044     1.00
        rename w                          30,000   30,000     1.00
          seqscan Works                   30,000   30,000     1.00
```

**q13** (4-way)

```
Node                                             Est.   Actual  q-error
-----------------------------------------------------------------------
project [e.name, p.pname, d.dname]                120      102     1.18
  nested_loop_join (e.eid = w.eid)                120      102     1.18
    project [d.dname, p.pname, w.eid]           1,200    1,418     1.18
      nested_loop_join (w.pid = p.pid)          1,200    1,418     1.18
        project [d.dname, p.pid, p.pname]          20       23     1.15
          nested_loop_join (p.did = d.did)         20       23     1.15
            project [d.did, d.dname]                2        2     1.00
              filter (d.budget < 500)               2        2     1.00
                rename d                           50       50     1.00
                  seqscan Dept                     50       50     1.00
            project [p.pid, p.did, p.pname]       500      500     1.00
              rename p                            500      500     1.00
                seqscan Proj                      500      500     1.00
        project [w.eid, w.pid]                 30,000   30,000     1.00
          rename w                             30,000   30,000     1.00
            seqscan Works                      30,000   30,000     1.00
    project [e.eid, e.name]                       993      796     1.25
      filter (e.age > 65)                         993      796     1.25
        rename e                               10,000   10,000     1.00
          seqscan Emp                          10,000   10,000     1.00
```

**q14** (group_by + HAVING)

```
Node                                                       Est.   Actual  q-error
---------------------------------------------------------------------------------
filter (count_star > 210)                                    17       10     1.70
  group_by [e.did; count(*), avg(e.salary)] mode=hash        50       50     1.00
    rename e                                             10,000   10,000     1.00
      seqscan Emp                                        10,000   10,000     1.00
```

**q15** (join + group_by)

```
Node                                        Est.   Actual  q-error
------------------------------------------------------------------
group_by [d.dname; count(*)] mode=sort        50       50     1.00
  project [e.did, d.did, d.dname]          3,877    3,877     1.00
    nested_loop_join (e.did = d.did)       3,877    3,877     1.00
      project [d.did, d.dname]                50       50     1.00
        rename d                              50       50     1.00
          seqscan Dept                        50       50     1.00
      project [e.did]                      3,877    3,877     1.00
        filter (e.age > 50)                3,877    3,877     1.00
          rename e                        10,000   10,000     1.00
            seqscan Emp                   10,000   10,000     1.00
```

**q16** (sort + limit)

```
Node                                Est.   Actual  q-error
----------------------------------------------------------
limit 10                              10       10     1.00
  sort [e.salary desc]             1,003       10   100.30  *
    project [e.name, e.salary]     1,003    1,003     1.00  *
      filter (e.did < 5)           1,003    1,003     1.00  *
        rename e                  10,000   10,000     1.00  *
          seqscan Emp             10,000   10,000     1.00  *
```

**q17** (distinct)

```
Node                               Est.   Actual  q-error
---------------------------------------------------------
distinct mode=sort                   50       50     1.00
  project [p.did]                   254      254     1.00
    filter (p.budget > 50000)       254      254     1.00
      rename p                      500      500     1.00
        seqscan Proj                500      500     1.00
```

**q18** (union)

```
Node                           Est.   Actual  q-error
-----------------------------------------------------
union                         1,735    1,466     1.18
  project [e.eid]               993      796     1.25
    filter (e.age > 65)         993      796     1.25
      rename e               10,000   10,000     1.00
        seqscan Emp          10,000   10,000     1.00
  project [w.eid]               742      747     1.01
    filter (w.hours = 40)       742      747     1.01
      rename w               30,000   30,000     1.00
        seqscan Works        30,000   30,000     1.00
```

**q19** (except)

```
Node                           Est.   Actual  q-error
-----------------------------------------------------
except                          196       22     8.91
  project [e.eid]               196      210     1.07
    filter (e.did = 3)          196      210     1.07
      rename e               10,000   10,000     1.00
        seqscan Emp          10,000   10,000     1.00
  project [w.eid]            14,965   14,965     1.00
    filter (w.hours > 20)    14,965   14,965     1.00
      rename w               30,000   30,000     1.00
        seqscan Works        30,000   30,000     1.00
```

**q20** (derived table)

```
Node                                          Est.   Actual  q-error
--------------------------------------------------------------------
rename t                                        11       10     1.10
  filter (e.did < 10)                           11       10     1.10
    group_by [e.did; count(*)] mode=hash        50       50     1.00
      rename e                              10,000   10,000     1.00
        seqscan Emp                         10,000   10,000     1.00
```
