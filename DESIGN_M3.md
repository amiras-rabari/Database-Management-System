# DESIGN_M3 — SQL front end

The SQL front end (`modules/sql`, plus the `sql` tool in `apps/`) turns SQL text into a **logical plan**. It has four steps: lexer → parser → name, type and aggregate checks → translator. The plan only says *what* to compute (`join`, never `hash_join`). Choosing algorithms and moving predicates is Module 4's job. The grammar is in `GRAMMAR_SQL.md`.

The spec (§M3.9) asks this document for three things. They are sections 1, 2 and 3:

1. Where duplicate elimination happens, and why
2. Scope and name resolution
3. Aggregate output naming rule
4. Type and aggregate checks
5. Translation order
6. CREATE TABLE, INSERT and the entry point
7. Decisions and known limits
8. Testing
9. Readings

Every plan and error message quoted below was printed by the built `sql` tool.

---

## 1. Where duplicate elimination happens, and why

SQL works on **bags**: the same row can appear many times, and a query keeps those copies unless it is told otherwise (§M3.4). So the translator removes duplicates **only where the SQL asks for it**, and nowhere else.

| SQL | Plan node | Removes duplicates? | Why |
|---|---|---|---|
| A table in FROM | `scan` | No | A table is a bag. Two equal rows stay two rows. |
| The select list | `project` | **No** | A projection that drops columns can make rows equal (two employees named Ann, age 30). SQL keeps both, so no `distinct` is added. |
| `SELECT DISTINCT` | one `distinct` **above** the `project` | Yes | Duplicates are judged on the *output* columns. Below the project the rows still have all their columns, so two "Ann" rows with different ids are not equal yet. |
| `UNION ALL` | `union_all` | No | It keeps every row of both sides. |
| `UNION` | `union` | Yes | SQL's UNION is a set union. |
| `INTERSECT`, `EXCEPT` | `intersect`, `except` | Yes | Set semantics (§M3.4): no duplicates in the result, even if the inputs have them. |
| `GROUP BY` | `group_by` | Yes, by construction | It makes one row per group, so the grouping columns are already unique. A `distinct` above it would do nothing, so **it is never emitted**, even for `SELECT DISTINCT … GROUP BY` (§M3.4: "do not emit it"). |

Examples (real `sql compile` output):
```
SELECT DISTINCT city FROM Emp
  distinct( project[Emp.city]( scan Emp ) )

SELECT DISTINCT did FROM Emp GROUP BY did
  project[Emp.did]( group_by[keys Emp.did]( scan Emp ) )        -- no distinct

SELECT city FROM Emp UNION ALL SELECT city FROM Dept   ->  union_all(...)
SELECT city FROM Emp UNION SELECT city FROM Dept       ->  union(...)
```
The two unions (example 5.7) have the same children. Only the root node differs, and both `sql parse --explain` and the plan JSON show it.

**Why `SELECT DISTINCT` is not free.** A `project` costs nothing: it changes each row as it passes through, with no page I/O of its own. A `distinct` has to compare every row with every other row. Our executor (M2) does that in one of two ways:
- **Sort mode:** it sorts the whole result with the external sort, then skips equal neighbours. For a result of M pages it writes all M pages as sorted runs and reads them back: about 2M extra page I/Os, more if the runs need several merge passes. It also blocks: no row comes out until the whole input has been read.
- **Hash mode:** if the result fits in memory, it costs nothing extra. Otherwise every page is written to partitions and read back: again about 2M extra.

`UNION` costs the same kind of work over both inputs, while `UNION ALL` just passes rows through. In the spec's words, the difference "is a sort of the whole result". So the translator never adds a `distinct` the SQL did not ask for. Module 4 chooses sort or hash mode; the SQL front end only places the node.

*Known difference from PostgreSQL:* with `SELECT DISTINCT` over a `GROUP BY` whose select list leaves out a grouping key (`SELECT DISTINCT COUNT(*) FROM Emp GROUP BY did`), PostgreSQL would remove equal counts. We follow §M3.4 and emit no `distinct`. In short , PostgreSQL removes duplicate count numbers when using GROUP BY, while this engine keeps every count even if numbers repeat.

---

## 2. Scope and name resolution

### 2.1 Building the scope
The scope is the list of columns that a name in this query can refer to. It is built from FROM, left to right:

1. Each table reference gets a **key**: its alias if one is written, else the table name.
2. Each column is re-labelled with that key. After `FROM Emp e` the scope has `e.name`, never `Emp.name`. Without an alias, the column is `Emp.name`.
3. Two entries with the same key are an error, at the second one:
   `FROM Emp, Emp` → `error: name: Table name or alias 'Emp' is used twice in FROM.` `FROM Emp e, Emp m` is fine (required case 12).
4. **A FROM subquery** must have an alias. It is translated on its own (it cannot see the outer query's names) and wrapped in `rename[alias]`. Its output columns, re-labelled with the alias, are the scope entry: in example 5.6 the outer query sees `t.did` and `t.n`, and nothing else. If two outputs would get the same name under the alias, that is `error: schema: Duplicate column 't.did'.` at the alias.
5. **JOIN … ON:** the ON after the k-th `JOIN` sees only the tables joined so far. A later alias is an error (`Unknown table or alias 'x' in 'x.did'.`), because the condition stays on its own `join` node, and that node's inputs are only those tables. PostgreSQL behaves the same way.

In the code, the scope is simply the schema of the FROM plan built so far (common's `requalify` and `concatSchemas`). No separate scope structure is needed.

### 2.2 Resolving one column
| Written | Rule | Error when it fails |
|---|---|---|
| Unknown table in FROM | The table must be in the catalog | `name: Unknown table 'Nope'.` |
| `t.c` | `t` must be a key in the scope | `name: Unknown table or alias 'p' in 'p.did'.` |
| | and `t` must have a column `c` | `name: Table 'e' has no column 'nope'.` |
| `c` | Exactly one scope column is named `c` | none: `name: Unknown attribute 'nope'.` |
| | | two or more: `ambiguity: Ambiguous unqualified attribute 'did' (matches Emp.did, Dept.did). Use a relation qualifier.` |

An unknown table and an unknown column give different messages (required case 11). An ambiguous name lists every candidate (required case 3). There is no "first match wins" rule.

### 2.3 `SELECT *`
`*` expands to every column of every scope entry, in FROM order and then column order, each one qualified. Shared names are not an error, because the qualifiers keep them apart: `SELECT * FROM Emp e, Dept d` gives `e.eid, …, e.did, …, d.did, …` (example 5.3, required case 2).

### 2.4 ORDER BY
ORDER BY first looks at the output names of the select list (an `AS` alias or an aggregate name), then at the FROM scope (§M3.3).
- `SELECT did, COUNT(*) AS n … ORDER BY n DESC` sorts by `n` (example 5.4).
- `SELECT name AS n FROM Emp ORDER BY n` sorts by `Emp.name`. A column alias only names the column for ORDER BY. The plan still writes the source column.
- **A column that is not in the select list** is allowed (required case 9). The `project` is widened with that column, the `sort` goes above it, and a final `project` keeps only the selected columns:
  ```
  SELECT name FROM Emp ORDER BY age
    project[Emp.name]( sort[Emp.age asc]( project[Emp.name, Emp.age]( scan Emp ) ) )
  ```
  Two cases have no correct plan, so they are errors:
  - with DISTINCT: `name: ORDER BY column 'Emp.age' must appear in the select list when DISTINCT is used.` One name can have many ages, so the order would not be defined. PostgreSQL refuses this too.
  - in an aggregating query: `aggregate: ORDER BY column 'Emp.age' must be a grouping column or an aggregate output.` Above the `group_by`, only keys and aggregates exist.

### 2.5 No unqualified name survives
Every column in the plan is fully qualified (`Emp.name`, `e.name`). The **one exception** is an aggregate's output name (`n`, `count_star`). It is a new column made by `group_by`, which has no table, and examples 5.4 and 5.6 write it bare (`select "n > 5"`, `project ["Emp.did","n"]`). A `rename` above it qualifies it (`t.n` in 5.6).

---

## 3. Aggregate output naming rule

Each aggregate in a logical plan is `[function, column, name]` (§M3.5). The name is:

1. **the `AS` alias**, if written: `COUNT(*) AS n` → `["count","*","n"]`;
2. otherwise **generated**: `count_star` for `COUNT(*)`, else `<function>_<qualifier>_<column>`:
   `AVG(salary)` → `avg_Emp_salary`, `MAX(e.name)` → `max_e_name`.

The generated names come from common's `aggregateOutputName`, the same rule exec uses for physical plans (§M2.2), so the two modules cannot disagree. The physical plan has no alias slot, so Module 4 uses the generated name there and renames the column for the output header.

Further rules:
- **One call, one entry.** The same aggregate written twice (in the select list and in HAVING) is one `group_by` entry. In 5.4 `HAVING COUNT(*) > 5` becomes `select "n > 5"` over the entry named `n`.
- **A HAVING-only aggregate** gets its own entry, which the final `project` then drops:
  ```
  SELECT did, COUNT(*), MAX(name) FROM Emp GROUP BY did HAVING SUM(salary) > 100
    project[Emp.did, count_star, max_Emp_name](
      select[sum_Emp_salary > 100](
        group_by[keys Emp.did; count(*) as count_star, max(Emp.name) as max_Emp_name,
                 sum(Emp.salary) as sum_Emp_salary]( scan Emp )))
  ```
- **Output types:** COUNT and SUM are INT, AVG is REAL (a decimal number), MIN and MAX keep the column's type.

---

## 4. Type and aggregate checks

**Types (§3.2):**
|                       Rule                              |               Error                                    |
|-----------------------------------------|---------------------------------------- |
| A comparison needs two values of the same type, `=` included. INT with AVG's REAL is allowed (both are numbers). | `type: Incompatible comparison operand types: INT and VARCHAR.` (`age > '30'`, required case 10) 

| SUM and AVG need an INT column. COUNT accepts anything; MIN and MAX accept either type. | `type: Aggregate 'avg' needs an INT column, but 'Emp.name' is VARCHAR(20).` 

| `*` only in COUNT. | `type: Only COUNT may use '*', not SUM(*).` 

| Set operators: same number of columns and the same type in each position. Names may differ; the result takes the left side's names. | `schema: Relation schemas are not union-compatible for 'union': column 1 is VARCHAR on the left but INT on the right.` at the operator (required case 14) 

**Aggregates (§3.1):** a query is *aggregating* if it has a GROUP BY or an aggregate in its select list. Then:
|                       Rule                              |               Error                                    |
|-----------------------------------------|---------------------------------------- |

| The select list may hold only grouping columns and aggregates. | `aggregate: Column 'Emp.name' must appear in GROUP BY or be used in an aggregate.` (required case 6) 

| HAVING may use only grouping columns and aggregates. | the same message, at the column in HAVING 

| No aggregate in WHERE (or ON): they run before grouping. | `aggregate: Aggregate COUNT(*) is not allowed in WHERE or ON: they run before grouping.` (required case 7) |

`SELECT COUNT(*) FROM Emp` with no GROUP BY is aggregating: `group_by` with no keys. That gives one row, even on an empty table (required case 13; exec's group_by test checks the empty-table half).

Checks run in plan order (FROM, WHERE, GROUP BY, select list, HAVING, ORDER BY), so the first error in that order is the one reported (§M3.6: first error only). Every error has the format `error: <category>: <message> at line L, column C`.

---

## 5. Translation order

Each select block is built bottom to top in §M3.5's order. **The translator makes no optimisation decisions.**

| Step | SQL | Plan node |
|---|---|---|
| 1 | FROM | `scan` per table; `rename` only when an alias is written; `cross` for a comma, `join` (with its ON) for JOIN; left to right, left-deep |
| 2 | WHERE | one `select` with the whole condition, unchanged |
| 3 | GROUP BY (or an aggregate in the select list) | `group_by` |
| 4 | HAVING | `select` above the `group_by` |
| 5 | select list | `project`, then `distinct` if DISTINCT was written (and there is no GROUP BY) |
| 6 | ORDER BY | `sort` |
| 7 | LIMIT | `limit` |
| 8 | UNION / INTERSECT / EXCEPT | applied between whole blocks, left associative |

Example 5.4, read bottom to top:
```
SELECT did, COUNT(*) AS n, AVG(salary) AS avg_sal FROM Emp WHERE age > 25
 GROUP BY did HAVING COUNT(*) > 5 ORDER BY n DESC LIMIT 10;

limit 10                                                        -- 7 LIMIT
  sort [["n","desc"]]                                           -- 6 ORDER BY
    project ["Emp.did","n","avg_sal"]                           -- 5 SELECT
      select "n > 5"                                            -- 4 HAVING
        group_by keys ["Emp.did"]                               -- 3 GROUP BY
                 aggregates [["count","*","n"],["avg","Emp.salary","avg_sal"]]
          select "Emp.age > 25"                                 -- 2 WHERE
            scan Emp                                            -- 1 FROM
```
WHERE sits below the grouping and HAVING above it (required case 5).

**No predicate is moved.** In 5.2 the WHERE `d.budget > 1000` stays above the join, even though it touches only one side. A comma join stays `cross` + `select` (5.3), while JOIN … ON gives `join` with the condition on it (5.2). Required case 15 shows the same four tables both ways: the plans differ only in where the predicates sit. Turning one form into the other and pushing predicates down are Module 4's rewrites.

---

## 6. CREATE TABLE, INSERT and the entry point

`compileSql(text, lookup)` compiles a script statement by statement and returns one result per statement. The sql module **never executes anything**: only storage touches files. The shell executes the results.

| Statement | Result | Checks (first error stops everything) |
|---|---|---|
| query | the logical plan | sections 2–5 |
| `CREATE TABLE` | table name + schema | common's `Schema` rule: VARCHAR(n) needs 1 ≤ n ≤ 255, no column twice (`schema`). "Table already exists" is reported by storage when the shell creates it. |
| `INSERT` | table name + one row | the table exists (`name`); one value per column, since there are no NULLs (`schema`); **the value's type is checked first**, so `'1'` is never accepted for an INT column (`type`); a string longer than VARCHAR(n) (`schema`) |

The catalog reaches sql only through `TableLookup`, a function from a table name to its schema. So the `db_sql` library does not link storage; the `sql` tool and the shell pass in a lookup over storage's catalog.

`sql compile` and `sql check` read a whole file but execute nothing. A SELECT on a table created earlier **in the same file** is therefore "Unknown table" there cause the script has not ran yet and hence table does not exist. The shell runs one statement at a time, so a CREATE is visible to the next statement.

**Commands (§M3.7):**
```
sql parse   --query "SELECT ..." --explain   # parse tree, no catalog needed
sql compile --db DIR --file q.sql --json     # logical plan JSON
sql check   --db DIR --file q.sql            # errors only
```
`check` prints nothing and exits 0 for a correct file. On an SQL error it prints the error and exits 1. A malformed command line exits 2 and an internal bug exits 3, the same as the storage and exec tools. Neither happens for a correct `check` command, so the spec's "0 or 1" holds.

---

## 7. Decisions and known limits

| Decision | Choice | Reason |
|---|---|---|
| Unqualified names in plans | Only aggregate output names (D1) | Fixed by examples 5.4 and 5.6. |
| Aggregate names | `AS` alias, else `count_star` / `avg_Emp_salary` (D2) | §M3.5 note; one rule shared with exec. |
| ORDER BY on a column that is not selected | Widened `project` → `sort` → final `project` (D3) | Keeps `sort` above `project` (§M3.5) and makes required case 9 work. |
| Column alias (`name AS n`) | Used only by ORDER BY; the plan writes `Emp.name` (D4) | The logical `project` has only `columns`, no alias field. |
| ON scope | Only the tables joined so far (D5) | The condition belongs to its own `join` node. |
| Duplicate output column (`SELECT name, name`) | `schema: Duplicate output column 'Emp.name'.` | Exec refuses such a schema too. |
| Same aggregate under two aliases (`COUNT(*) AS a, COUNT(*) AS b`) | `schema: Aggregates 'a' and 'b' are the same aggregate (count_star); select it once.` | A physical plan names both `count_star`, which exec refuses, so the plan could never run (§M3.10). PostgreSQL accepts it and returns two columns. |
| Plan depth | The parser allows 1000 SQL levels; the finished plan must also fit common's 1000-level limit, else `syntax: Query nested too deep: its plan has more than 1000 levels.` | One subquery is 2 plan levels (rename + project). Without this check a 500-deep query gave JSON that opt and exec refuse, and a 1000-deep one crashed `sql compile`. |
| Exit codes | 0 / 1 / 2 / 3 as in section 6 | Same as the storage and exec tools. |

**Known limits** (the spec is silent; PostgreSQL differs):
- `(SELECT name AS n FROM Emp) AS t … t.n` is a `name` error, because the alias does not reach the plan (D4). PostgreSQL accepts it.
- An aggregate alias equal to a grouping column's name (`SELECT did, COUNT(*) AS did … GROUP BY did`) makes a later bare `did` ambiguous.
- ORDER BY and LIMIT belong to one select block (the §M3.2 grammar), never to a whole UNION (`GRAMMAR_SQL.md` D10).
- The instructor will not post a sample catalog (Brightspace, 2026-11-11: use your own and document it). The tests use our own catalogs: an in-memory one (`modules/sql/tests/test_catalog.h`: Emp and Dept with the columns of the §M3.5 examples, plus Proj and Loc for required cases 2 and 15), and `--db` folders built with storage's `createTable`.

---

## 8. Testing

All tests are in `modules/sql/tests/` and `apps/tests/sql_cli_tests.cpp`, and all pass.
- **The 15 required cases (§M3.8):** each has a test whose name contains "required case N" (`sql_tests -tc="*required case*"` lists them). The case-to-test table is in `modules/sql/STATUS.md`.
- **Examples 5.1–5.7:** 5.1 and 5.2 are compared byte for byte with the spec's JSON. The others are compared as whole plans.
- **Every plan** in these tests is checked with common's `outputSchema` (all names and conditions bind) and written to JSON and read back with common's logical reader. That proves it is a valid logical plan: logical op names only, 3-element aggregates.
- **Mutation checks:** each one deliberately broke the code and turned tests red:
  - WHERE moved below the join;
  - a qualifier dropped;
  - DISTINCT placed above GROUP BY;
  - UNION ALL emitted as `union`;
  - first-match column resolution;
  - the INSERT type check skipped;
  - `check` exiting 0 on an error.

---

## 9. Readings

 Architecture based on my own relational parser built before which was completely inspired by the following sources and some publicly  available info online 

- Aho, A. V., Lam, M. S., Sethi, R., & Ullman, J. D. *Compilers: Principles, Techniques, and Tools* (Dragon Book, 2nd ed.), Sections 2.2–2.4, 4.4. Consulted for formal EBNF grammar design, precedence stratification, and left-recursion elimination techniques.
- Wikipedia articles: "Extended Backus–Naur form", "Recursive descent parser", "Maximal munch", and "Operator-precedence parser".
- **Nystrom, *Crafting Interpreters*, chapters "Scanning" and "Parsing Expressions".**
  - One function per precedence level (`parseOr → parseAnd → parseNot → parsePrimary`) puts NOT > AND > OR in the grammar, not in the parser code, as §M3.2 requires.
  - Loops instead of left recursion make the set operators and AND/OR chains left-associative. Reporting the first error at the unexpected token is panic mode (§M3.6).
- **PostgreSQL documentation, `SELECT`.**
  - ORDER BY resolves output column names before input columns, which matches §M3.3.
  - It is also the reference for the known differences in sections 1 and 7: INTERSECT binding tighter there, DISTINCT over GROUP BY, ORDER BY over a whole UNION.
- AI assistance was taken for refinement and clean document writing based on general best practices However the grammar is heavily influenced by the textbook listed above

