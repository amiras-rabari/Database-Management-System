# REPORT — Integration: the hidden workload

Status: **rehearsal**. The grader publishes the real dataset and its 25 queries 72 hours before the demo. Until then this report runs the same steps on our own data. On release day the tables below are replaced by the hidden workload's numbers (release-day checklist in `modules/shell/STATUS.md`).

For each query the integration spec asks for four numbers: the **result row count**, the **page I/O**, the **wall time**, and the **estimated against actual rows at the root**. Correctness is checked by running the same queries on PostgreSQL over the same data. In the spec's words: "Your system does not need to be fast. It needs to be right, and your numbers need to explain themselves."

## 1. Setup

- **Program:** the `shell` (one binary: sql → opt → exec → storage), built on Windows with the project's three build commands.
- **Memory budget:** B = 4 frames for every query, the smallest B used in grading.
- **Cold pool:** before every query the shell empties the buffer pool and sets the counters to zero (`resetPool`). So a query's numbers never depend on what ran before it.
- **Where the numbers come from:**
  - Rows: the number of result rows the plan returned.
  - Page I/O: exec's plan totals, page reads + page writes summed over every operator. A page found in the pool costs nothing. Temporary run pages written and read by sort and hash count too.
  - Wall time: `steady_clock` from the start of compiling the statement to its last row, in milliseconds. Wall time is the one number that changes between runs.
  - Estimated root rows: opt's estimate for the plan's top node (`estimates[0]`). Actual root rows: the rows the top node really returned.
- **Determinism check:** each query runs twice inside one shell. Every number except the wall time must be the same both times, or the script stops (§0.2). Proven to catch a fault: with the cold pool removed, the script stopped at q24.
- **Commands:**
  ```
  python bench/report_integration.py rehearse
  python bench/report_integration.py run bench/out/integration/rehearsal --frames 4
  python bench/report_integration.py compare bench/out/integration/rehearsal   # on OpenStack
  ```

### Assumptions (rehearsal data)
- **Data:** opt's M4 dataset (`bench/report_m4.py`, `make_tables()`, seed 3005). It has six tables: Dept (50 rows), Emp (10,000), Proj (500), Works (30,000), Cust (32,000, with a city that is fixed by the zip code), Orders (96,000). It also has exec's sort-merge example tables R (R.b = 1, 2, 2, 3) and S (S.b = 2, 2, 4). Every table is loaded with `LOAD` and analyzed with `ANALYZE` (20 histogram buckets).
- **Queries:** opt's twenty queries (q01–q20) and five more (q21–q25), 25 in all, as in the hidden workload:
  - q21 UNION ALL; q22 INTERSECT; q23 an empty result;
  - q24 exec's sort-merge example `SELECT * FROM R JOIN S ON R.b = S.b`;
  - q25 COUNT, AVG, MIN and MAX with no GROUP BY.

## 2. Per-query numbers (B = 4)

| Query | What it tests | Rows | Page I/O (reads + writes) | Wall time (ms) | Estimated root rows | Actual root rows |
|---|---|---:|---:|---:|---:|---:|
| q01 | equality | 207 | 75 | 25.5 | 205 | 207 |
| q02 | range | 1761 | 75 | 26.4 | 1969 | 1761 |
| q03 | AND of equality and range | 32 | 75 | 25.5 | 41 | 32 |
| q04 | OR | 1849 | 75 | 27.2 | 2016 | 1849 |
| q05 | AND on the correlated pair | 1618 | 747 | 154.8 | 82 | 1618 |
| q06 | 2-way join, commas | 1965 | 77 | 40.5 | 2000 | 1965 |
| q07 | 2-way join, JOIN ON | 3749 | 353 | 887.5 | 3749 | 3749 |
| q08 | 2-way join, JOIN ON | 35 | 7 | 3.9 | 38 | 35 |
| q09 | 2-way join, commas | 2726 | 124 | 149.5 | 2820 | 2726 |
| q10 | AND on one table | 336 | 543 | 206.4 | 327 | 336 |
| q11 | 3-way join | 848 | 282 | 734.4 | 1260 | 848 |
| q12 | 3-way join | 62 | 198 | 117.8 | 61 | 62 |
| q13 | 4-way join | 102 | 521 | 309.4 | 120 | 102 |
| q14 | GROUP BY + HAVING | 10 | 75 | 48.1 | 17 | 10 |
| q15 | join + GROUP BY | 50 | 77 | 54.4 | 50 | 50 |
| q16 | ORDER BY + LIMIT | 10 | 82 | 29.9 | 10 | 10 |
| q17 | DISTINCT | 50 | 5 | 3.0 | 50 | 50 |
| q18 | UNION | 1466 | 202 | 72.6 | 1735 | 1466 |
| q19 | EXCEPT | 22 | 375 | 163.2 | 196 | 22 |
| q20 | derived table | 10 | 75 | 37.9 | 11 | 10 |
| q21 | UNION ALL | 963 | 194 | 67.1 | 1158 | 963 |
| q22 | INTERSECT | 28 | 202 | 75.9 | 196 | 28 |
| q23 | empty result | 0 | 75 | 25.6 | 1 | 0 |
| q24 | sort-merge example | 4 | 4 | 1.4 | 4 | 4 |
| q25 | aggregates, no GROUP BY | 1 | 75 | 28.1 | 1 | 1 |

A one-table scan of Emp costs 75 pages: 74 data pages plus 1 directory page. That is why q01–q04, q14, q20, q23 and q25 all show 75.

## 3. Why some estimates are far off

The q-error of a query is the larger of (estimated, actual) divided by the smaller. Most queries are within 1.5×. Every query with a larger gap is explained below with its `EXPLAIN ANALYZE`, which puts the estimate and the real count of every plan node side by side.

- **q05 (82 estimated, 1618 actual, 20×).** `EXPLAIN ANALYZE` shows the whole error in one node: `filter (c.city = 'City00' and c.zip < 50)`, est 82, actual 1618. The optimizer multiplies the two selectivities as if the conditions were independent: 1/20 for the city and about 1/20 for the zip, so 32,000 × 1/400 ≈ 82. In this data the city is fixed by the zip (zips 0–49 are exactly City00), so the zip condition already implies the city one, and the true fraction is 1/20. This is the known limit of the independence assumption (`REPORT_M4.md`, "where it breaks").
- **q19 (196 estimated, 22 actual, 9×).** The left input's estimate is right (`filter (e.did = 3)`: est 196, actual 210). The error is at `except`. With no statistics on how two inputs overlap, opt's rule is "EXCEPT keeps the left input" (opt `INTERFACE.md`), which is an upper bound. In fact 188 of the 210 employees of department 3 also work more than 20 hours on some project, so only 22 are left.
- **q22 (196 estimated, 28 actual, 7×).** The same kind of upper bound: `intersect` is estimated as the smaller input, min(196, 1483) = 196. Only 28 of department 3's employee ids appear in the 1,483 long-hours rows.
- **q14 (17 estimated, 10 actual, 1.7×).** HAVING `COUNT(*) > 210` filters on a count that only exists after grouping, so there are no statistics on it. The optimizer uses its default range guess of 1/3 of the 50 groups (rounded up to 17).
- **q11 (1260 estimated, 848 actual, 1.5×).** The error comes from the bottom: `filter (p.budget > 95000)` is estimated at 21 rows and is really 14. The histogram bucket that holds 95,000 is assumed to be evenly filled, and it is not. The joins above carry the same 1.5× upward.
- **q23 (1 estimated, 0 actual).** Every estimate is at least 1 row (opt rounds up, `ceilRows`), so an empty result always shows a gap of one row.
- **q21 and q18 (1.2×).** Both are estimated as left + right. For UNION ALL (q21) that is the right rule; the gap comes from the two filter estimates below it, each a little high. UNION (q18) has the same filter gap, and it also removes duplicates, which left + right does not count.

## 4. Correctness against PostgreSQL

**To do on the OpenStack instance** (no PostgreSQL on the Windows machine): run `compare` and paste its pass/fail table here.

How the check works:
- A fresh PostgreSQL database is created with the **C locale**, so strings compare byte by byte, the same order as ours.
- The same CSV files are loaded with `\copy … WITH (FORMAT csv, NULL '\N')`, so an empty field stays an empty string as in our loader (no NULLs).
- Each query file runs unchanged with `psql -A -t -F '|'`.

Only these differences are normalised. None of them hides a wrong row:
- **Layout:** our header line and `N rows` line are dropped, and ` | ` becomes `|`. These are printing differences only.
- **Order:** without ORDER BY a result is a bag (no defined order), so both outputs are sorted before comparing. With ORDER BY they are compared in order. Rows that tie on the sort key (q16's `LIMIT 10` could cut through a tie) are checked by hand if they differ.
- **AVG:** PostgreSQL prints many decimals. Both sides are rounded to 4 decimals, the way we print AVG (DECISIONS 2026-11-07). They can differ only on an exact halfway value.
- **SUM above the INT range:** this is an error for us and a large number for PostgreSQL (DECISIONS 2026-11-07). Such a query would show as a failure with our error text and be explained here. No rehearsal query reaches it.

Checked so far (locally, without PostgreSQL): the comparison accepts q25's AVG printed with PostgreSQL's many decimals, and it fails when one value differs. **Still to do on OpenStack:** change one value only on the PostgreSQL side and see exactly the queries that touch that row fail.
