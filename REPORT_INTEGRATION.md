# REPORT — Integration

We ran 25 queries through the whole system (the `shell`: sql → opt → exec → storage) and recorded four numbers for each one, as the integration spec asks:

| Number | Meaning |
|---|---|
| **Rows** | how many rows the query returned |
| **Page I/O** | pages read + pages written while it ran |
| **Wall time** | milliseconds from compiling the query to its last row |
| **Estimated vs actual** | rows the optimizer predicted at the top of the plan, against the rows that really came out |

> "Your system does not need to be fast. It needs to be right, and your numbers need to explain themselves." — integration spec

---

## 1. Setup

| | |
|---|---|
| **Memory** | $B = 4$ frames for every query (the smallest B used in grading) |
| **Cold start** | the buffer pool is emptied before each query, so no query benefits from the one before |
| **Repeatable** | each query runs twice; every number except wall time must match, or the run stops |

How the numbers are computed:

$$
\text{page I/O} = \text{page reads} + \text{page writes}
\qquad\qquad
\text{q-error} = \frac{\max(\text{estimated},\ \text{actual})}{\min(\text{estimated},\ \text{actual})}
$$

A q-error of $1$ means the estimate was exact; $2$ means it was off by a factor of two.

To reproduce:

```bash
python bench/report_integration.py generate
python bench/report_integration.py run bench/out/integration/workload --frames 4
```

### Data

Our own dataset, generated with a fixed seed (3005), so every run produces the same files.

| Table | Rows | Notes |
|---|---:|---|
| Dept | 50 | |
| Emp | 10,000 | 74 data pages |
| Proj | 500 | |
| Works | 30,000 | |
| Cust | 32,000 | city is fixed by zip code (a correlated pair, on purpose) |
| Orders | 96,000 | |
| R, S | 4, 3 | exec's sort-merge example: R.b = 1, 2, 2, 3 and S.b = 2, 2, 4 |

Every table is loaded with `LOAD` and analyzed with `ANALYZE` (20 histogram buckets).

---

## 2. Queries and results

### 2.1 Selections on one table

```sql
-- q01  equality
SELECT * FROM Emp e WHERE e.did = 7;

-- q02  range
SELECT e.name, e.salary FROM Emp e WHERE e.age > 60;

-- q03  AND of equality and range
SELECT e.eid FROM Emp e WHERE e.did = 7 AND e.age > 60;

-- q04  OR
SELECT e.eid, e.age FROM Emp e WHERE e.age < 25 OR e.salary > 120000;

-- q05  AND on the correlated pair
SELECT c.cid, c.name FROM Cust c WHERE c.city = 'City00' AND c.zip < 50;

-- q10  AND on a large table
SELECT o.oid, o.amount FROM Orders o WHERE o.amount > 990 AND o.status = 'paid';
```

| Query | Rows | Page I/O | Time (ms) | Estimated | Actual |
|---|---:|---:|---:|---:|---:|
| q01 | 207 | 75 | 25.5 | 205 | 207 |
| q02 | 1,761 | 75 | 26.4 | 1,969 | 1,761 |
| q03 | 32 | 75 | 25.5 | 41 | 32 |
| q04 | 1,849 | 75 | 27.2 | 2,016 | 1,849 |
| q05 | 1,618 | 747 | 154.8 | **82** | **1,618** |
| q10 | 336 | 543 | 206.4 | 327 | 336 |

Every Emp scan costs the same: $74$ data pages $+\ 1$ directory page $= 75$.

### 2.2 Joins

```sql
-- q06  2-way, comma syntax
SELECT e.name, d.dname FROM Emp e, Dept d WHERE e.did = d.did AND d.budget > 4000;

-- q07  2-way, JOIN ON
SELECT e.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE w.hours > 35;

-- q08  2-way, JOIN ON
SELECT p.pname, d.dname FROM Proj p JOIN Dept d ON p.did = d.did WHERE p.budget < 10000;

-- q09  2-way, comma syntax
SELECT w.eid, p.pname FROM Works w, Proj p WHERE w.pid = p.pid AND p.budget > 90000;

-- q11  3-way
SELECT e.name, p.pname
FROM Emp e JOIN Works w ON e.eid = w.eid
           JOIN Proj p  ON w.pid = p.pid
WHERE p.budget > 95000;

-- q12  3-way
SELECT e.name, w.hours
FROM Dept d JOIN Emp e   ON d.did = e.did
            JOIN Works w ON e.eid = w.eid
WHERE d.dname = 'dept07' AND w.hours < 5;

-- q13  4-way
SELECT e.name, p.pname, d.dname
FROM Emp e JOIN Works w ON e.eid = w.eid
           JOIN Proj p  ON w.pid = p.pid
           JOIN Dept d  ON p.did = d.did
WHERE d.budget < 500 AND e.age > 65;

-- q24  exec's sort-merge example
SELECT * FROM R JOIN S ON R.b = S.b;
```

| Query | Rows | Page I/O | Time (ms) | Estimated | Actual |
|---|---:|---:|---:|---:|---:|
| q06 | 1,965 | 77 | 40.5 | 2,000 | 1,965 |
| q07 | 3,749 | 353 | 887.5 | 3,749 | 3,749 |
| q08 | 35 | 7 | 3.9 | 38 | 35 |
| q09 | 2,726 | 124 | 149.5 | 2,820 | 2,726 |
| q11 | 848 | 282 | 734.4 | 1,260 | 848 |
| q12 | 62 | 198 | 117.8 | 61 | 62 |
| q13 | 102 | 521 | 309.4 | 120 | 102 |
| q24 | 4 | 4 | 1.4 | 4 | 4 |

q24 matches the spec's worked example exactly: $2 \times 2 = 4$ matching rows.

### 2.3 Grouping, sorting and DISTINCT

```sql
-- q14  GROUP BY + HAVING
SELECT e.did, COUNT(*) AS n, AVG(e.salary) AS avg_sal
FROM Emp e GROUP BY e.did HAVING COUNT(*) > 210;

-- q15  join + GROUP BY
SELECT d.dname, COUNT(*) AS n
FROM Emp e JOIN Dept d ON e.did = d.did
WHERE e.age > 50 GROUP BY d.dname;

-- q16  ORDER BY + LIMIT
SELECT e.name, e.salary FROM Emp e WHERE e.did < 5 ORDER BY e.salary DESC LIMIT 10;

-- q17  DISTINCT
SELECT DISTINCT p.did FROM Proj p WHERE p.budget > 50000;

-- q20  derived table
SELECT t.did, t.n
FROM (SELECT e.did, COUNT(*) AS n FROM Emp e GROUP BY e.did) AS t
WHERE t.did < 10;

-- q25  aggregates without GROUP BY
SELECT COUNT(*), AVG(e.salary), MIN(e.name), MAX(e.age) FROM Emp e WHERE e.did = 7;
```

| Query | Rows | Page I/O | Time (ms) | Estimated | Actual |
|---|---:|---:|---:|---:|---:|
| q14 | 10 | 75 | 48.1 | 17 | 10 |
| q15 | 50 | 77 | 54.4 | 50 | 50 |
| q16 | 10 | 82 | 29.9 | 10 | 10 |
| q17 | 50 | 5 | 3.0 | 50 | 50 |
| q20 | 10 | 75 | 37.9 | 11 | 10 |
| q25 | 1 | 75 | 28.1 | 1 | 1 |

### 2.4 Set operations

```sql
-- q18  UNION (removes duplicates)
SELECT e.eid FROM Emp e WHERE e.age > 65
UNION
SELECT w.eid FROM Works w WHERE w.hours = 40;

-- q21  UNION ALL (keeps duplicates)
SELECT e.eid FROM Emp e WHERE e.age > 68
UNION ALL
SELECT w.eid FROM Works w WHERE w.hours = 40;

-- q19  EXCEPT
SELECT e.eid FROM Emp e WHERE e.did = 3
EXCEPT
SELECT w.eid FROM Works w WHERE w.hours > 20;

-- q22  INTERSECT
SELECT e.eid FROM Emp e WHERE e.did = 3
INTERSECT
SELECT w.eid FROM Works w WHERE w.hours > 38;
```

| Query | Rows | Page I/O | Time (ms) | Estimated | Actual |
|---|---:|---:|---:|---:|---:|
| q18 | 1,466 | 202 | 72.6 | 1,735 | 1,466 |
| q21 | 963 | 194 | 67.1 | 1,158 | 963 |
| q19 | 22 | 375 | 163.2 | **196** | **22** |
| q22 | 28 | 202 | 75.9 | **196** | **28** |

### 2.5 Empty result

```sql
-- q23  no row matches
SELECT e.name FROM Emp e WHERE e.age > 100;
```

| Query | Rows | Page I/O | Time (ms) | Estimated | Actual |
|---|---:|---:|---:|---:|---:|
| q23 | 0 | 75 | 25.6 | 1 | 0 |

---

## 3. Why some estimates miss

Most queries are within a q-error of $1.5$. The larger gaps below are each traced to one plan node with `EXPLAIN ANALYZE`, which prints the estimate and the real count side by side.

### q05 — correlated columns (q-error ≈ 20)

The optimizer assumes `city` and `zip` are independent, so it multiplies their selectivities:

$$
\hat{n} = 32{,}000 \times \underbrace{\tfrac{1}{20}}_{\text{city}} \times \underbrace{\tfrac{1}{20}}_{\text{zip} < 50} \approx 82
\qquad\text{but}\qquad
n = 1{,}618
$$

In our data the zip decides the city (zips 0–49 are exactly City00), so the second condition removes nothing. The true fraction is $\tfrac{1}{20}$, not $\tfrac{1}{400}$. This is the known limit of the independence assumption, and why we built this pair (`REPORT_M4.md`).

### q19 and q22 — set operations (q-error 9 and 7)

The optimizer has no statistics on how two inputs overlap, so it uses safe upper bounds:

$$
\hat{n}_{\text{EXCEPT}} = \hat{n}_{\text{left}} = 196
\qquad\qquad
\hat{n}_{\text{INTERSECT}} = \min(196,\ 1{,}483) = 196
$$

The left input itself is estimated well (196 vs 210 employees in department 3). The overlap is what the bound misses:

- **EXCEPT:** 188 of those 210 also work more than 20 hours somewhere, so only 22 are left.
- **INTERSECT:** only 28 of them work more than 38 hours.

### q14 — HAVING on a count (q-error 1.7)

`COUNT(*) > 210` filters a value that only exists after grouping, so there are no statistics for it. The optimizer uses its default range guess of one third:

$$
\hat{n} = \left\lceil 50 \times \tfrac{1}{3} \right\rceil = 17
\qquad\text{vs}\qquad n = 10
$$

### q11 — a histogram bucket (q-error 1.5)

The miss starts at the bottom of the plan: `p.budget > 95000` is estimated at 21 rows and is really 14, because the bucket holding 95,000 is not evenly filled. Both joins above carry the same factor $1.5$ upward.

### q18 and q21 — UNION (q-error 1.2)

Both are estimated as $\hat{n}_{\text{left}} + \hat{n}_{\text{right}}$. The two filters below are each a little high. UNION (q18) also removes duplicates, which that sum does not count.

### q23 — empty result

Every estimate is rounded up to at least one row, so an empty result always shows $1$ vs $0$.

---

## 4. Correctness against PostgreSQL

The same data and the same 25 query files are run on PostgreSQL on our OpenStack instance, and the rows are compared with ours.

```bash
python bench/report_integration.py compare bench/out/integration/workload
```

- The PostgreSQL database uses the **C locale**, so text sorts byte by byte, the same as ours.
- Each query file runs unchanged.

Only printing differences are evened out before comparing:

| Difference | How it is handled |
|---|---|
| Our header line and `N rows` line | dropped; ` \| ` becomes `\|` |
| Row order without ORDER BY | both sides sorted (a result is a bag, with no fixed order) |
| AVG decimals | both rounded to 4 places, as we print them |

**Result:** _pending, to be added after the run on OpenStack._
