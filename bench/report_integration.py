"""The integration workload report and the PostgreSQL check (integration SPEC).

The workload is 25 queries over our own dataset. For
each query we report the result row count, the page I/O, the wall time, and
the estimated against actual rows at the root. Correctness is checked by
running the same queries on PostgreSQL over the same data.

A workload is a folder holding:
    setup.sql        CREATE TABLE, `LOAD t FROM 'data/t.csv'` and `ANALYZE t`
                     for each table, one statement per line; CSV paths are
                     relative to the folder
    queries/qNN.sql  one query per file

Usage (from the repo root, after building):
    python bench/report_integration.py generate [--bin build/apps]
        writes our workload to bench/out/integration/workload/
    python bench/report_integration.py run FOLDER [--frames 4] [--bin build/apps]
        (our machine) loads FOLDER with the `shell`, runs every query twice in
        one shell with --json (the counters must be identical, §0.2), saves each query's
        rows to bench/out/integration/ours/qNN.txt and prints the Markdown table
    python bench/report_integration.py compare FOLDER [--ours DIR] [--psql psql]
        (the OpenStack instance) loads FOLDER into a fresh PostgreSQL database
        with the C locale, runs every query with psql and compares the rows
        with ours; prints pass/fail per query

Only these differences are normalised, each one explained in
REPORT_INTEGRATION.md; everything else is a real mismatch:
- our header line and "N rows" line are dropped, " | " becomes "|";
- without ORDER BY both results are bags: the lines are sorted first;
- a number with a decimal point (AVG) is rounded to 4 decimals on both sides,
  the way we print it (DECISIONS 2026-11-07).
A SUM above the INT range is an error for us and a big number for PostgreSQL
(DECISIONS 2026-11-07): such a query shows as a failure with our error text.
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

import report_m4  # the opt dataset and queries (make_tables, QUERIES)

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "bench" / "out" / "integration"
DB = OUT / "db"
PG_DATABASE = "comp3005_check"


# ---------------------------------------------------------------- the workload

# Opt's twenty queries already cover selections, 2- to 4-way joins, GROUP BY
# with HAVING, ORDER BY with LIMIT, DISTINCT, UNION, EXCEPT and a derived
# table. These five add what is missing, for 25 queries in all.
EXTRA_QUERIES = {
    "q21": "SELECT e.eid FROM Emp e WHERE e.age > 68 UNION ALL SELECT w.eid FROM Works w WHERE w.hours = 40",
    "q22": "SELECT e.eid FROM Emp e WHERE e.did = 3 INTERSECT SELECT w.eid FROM Works w WHERE w.hours > 38",
    "q23": "SELECT e.name FROM Emp e WHERE e.age > 100",
    "q24": "SELECT * FROM R JOIN S ON R.b = S.b",
    "q25": "SELECT COUNT(*), AVG(e.salary), MIN(e.name), MAX(e.age) FROM Emp e WHERE e.did = 7",
}

# Exec's sort-merge example (§M2.3): R.b = 1, 2, 2, 3 and S.b = 2, 2, 4 give 4 rows.
EXTRA_TABLES = {
    "R": ("a:INT,b:INT", [(1, 1), (2, 2), (3, 2), (4, 3)]),
    "S": ("a:INT,b:INT", [(10, 2), (20, 2), (30, 4)]),
}


def generate():
    """Writes the workload: opt's tables and queries (seed 3005) plus the five
    above, as a folder with setup.sql and queries/qNN.sql."""
    folder = OUT / "workload"
    shutil.rmtree(folder, ignore_errors=True)
    (folder / "data").mkdir(parents=True)
    (folder / "queries").mkdir()
    tables = {name: t for name, t in report_m4.make_tables().items() if name not in ("A", "Bp", "C")}
    tables.update(EXTRA_TABLES)
    setup = []
    for name, (schema, rows) in tables.items():
        (folder / "data" / f"{name}.csv").write_text("".join(",".join(map(str, r)) + "\n" for r in rows),
                                                     encoding="utf-8")
        # "did:INT,dname:VARCHAR(12)" -> "did INT, dname VARCHAR(12)"
        columns = ", ".join(item.replace(":", " ") for item in schema.split(","))
        setup += [f"CREATE TABLE {name} ({columns});", f"LOAD {name} FROM 'data/{name}.csv';", f"ANALYZE {name};"]
    (folder / "setup.sql").write_text("\n".join(setup) + "\n", encoding="utf-8")
    queries = {q: sql for q, (_, sql) in report_m4.QUERIES.items()}
    queries.update(EXTRA_QUERIES)
    for query, sql in queries.items():
        (folder / "queries" / f"{query}.sql").write_text(sql + ";\n", encoding="utf-8")
    print(f"workload written to {folder}")


# ---------------------------------------------------------------- run (ours)

def shell(bin_dir, folder, *args, stdin=""):
    """Runs our `shell` in FOLDER (so the CSV paths in setup.sql work)."""
    return subprocess.run([str(bin_dir / "shell"), "--db", str(DB), *map(str, args)], cwd=folder, input=stdin,
                          capture_output=True, text=True, encoding="utf-8")


def run(folder, frames, bin_dir):
    shutil.rmtree(DB, ignore_errors=True)
    shutil.rmtree(OUT / "ours", ignore_errors=True)
    (OUT / "ours").mkdir(parents=True)
    setup = shell(bin_dir, folder, "--frames", frames, "--file", "setup.sql")
    if setup.returncode != 0:
        sys.exit(f"setup.sql failed:\n{setup.stderr}")

    lines = [f"Integration workload at B = {frames} (cold buffer pool before every query).", "",
             "| Query | Rows | Page I/O (reads + writes) | Wall time (ms) | Estimated root rows | Actual root rows |",
             "|---|---:|---:|---:|---:|---:|"]
    for path in sorted((folder / "queries").glob("*.sql")):
        query = path.stem
        sql_file = path.resolve()
        # Twice with --json in one shell: every number but the wall time must
        # repeat (§0.2). In one process the second run finds the first one's
        # pages cached unless the shell empties the pool before each query.
        # (A query file may lack its final ';', so one is put on each copy.)
        sql = path.read_text(encoding="utf-8").strip().rstrip(";") + ";\n"
        result = shell(bin_dir, folder, "--frames", frames, "--json", stdin=sql + sql)
        numbers = [json.loads(line) for line in result.stdout.splitlines()]
        if not numbers[0]["ok"]:
            lines.append(f"| {query} | error: {numbers[0]['error']} | | | | |")
        else:
            first, second = (dict(n, wall_ms=None, statement=None) for n in numbers)
            if first != second:
                sys.exit(f"{query}: the counters changed between two runs (determinism, GLOBAL 0.2)")
            n = numbers[0]
            lines.append(f"| {query} | {n['rows']} | {n['page_reads'] + n['page_writes']} | {n['wall_ms']:.1f} "
                         f"| {n['est_root_rows']} | {n['actual_root_rows']} |")
        # Once in plain mode: the rows, for the PostgreSQL check.
        plain = shell(bin_dir, folder, "--frames", frames, "--file", sql_file)
        (OUT / "ours" / f"{query}.txt").write_text(plain.stdout + plain.stderr, encoding="utf-8")
    text = "\n".join(lines) + "\n"
    (OUT / "table.md").write_text(text, encoding="utf-8")
    print(text)


# ---------------------------------------------------------------- compare (PostgreSQL)

def number_fixed(field):
    """A field with a decimal point that is a number (AVG) -> 4 decimals, the
    way we print it (std::to_chars, like printf %.4f). Anything else as is."""
    if "." not in field:
        return field
    try:
        return f"{float(field):.4f}"
    except ValueError:
        return field


def normalise(rows, ordered):
    """rows: one list of fields per row. Bags (no ORDER BY) are sorted."""
    rows = [tuple(number_fixed(f) for f in row) for row in rows]
    return rows if ordered else sorted(rows)


def our_rows(text):
    """Our plain output: header, rows joined by " | ", then "N rows". None if
    the query failed (the text is then our error)."""
    lines = text.splitlines()
    if not lines or not (lines[-1].endswith(" rows") or lines[-1].endswith(" row")):
        return None
    return [line.split(" | ") for line in lines[1:-1]]


def psql(program, database, *args, cwd=None, stdin=None):
    return subprocess.run([program, "-X", "-q", "-v", "ON_ERROR_STOP=1", "-d", database, *args], cwd=cwd,
                          input=stdin, capture_output=True, text=True, encoding="utf-8")


def compare(folder, ours, program):
    # A fresh database with the C locale: strings compare byte by byte, like ours.
    for command in (f"DROP DATABASE IF EXISTS {PG_DATABASE}",
                    f"CREATE DATABASE {PG_DATABASE} TEMPLATE template0 LC_COLLATE 'C' LC_CTYPE 'C'"):
        result = psql(program, "postgres", "-c", command)
        if result.returncode != 0:
            sys.exit(f"psql failed: {command}\n{result.stderr}")
    # setup.sql for PostgreSQL: `LOAD t FROM 'f';` becomes psql's \copy. In
    # CSV format an empty unquoted field is NULL; NULL '\N' keeps it an empty
    # string, as our loader does (no NULLs anywhere, §0.2).
    setup = []
    for line in (folder / "setup.sql").read_text(encoding="utf-8").splitlines():
        words = line.strip().rstrip(";").split()
        if len(words) == 4 and words[0].upper() == "LOAD" and words[2].upper() == "FROM":
            setup.append(f"\\copy {words[1]} FROM {words[3]} WITH (FORMAT csv, NULL '\\N')")
        else:
            setup.append(line)
    result = psql(program, PG_DATABASE, cwd=folder, stdin="\n".join(setup) + "\n")
    if result.returncode != 0:
        sys.exit(f"loading PostgreSQL failed:\n{result.stderr}")

    lines = ["| Query | Result |", "|---|---|"]
    passed = 0
    queries = sorted((folder / "queries").glob("*.sql"))
    for path in queries:
        query = path.stem
        ordered = "ORDER BY" in " ".join(path.read_text(encoding="utf-8").upper().split())
        mine = our_rows((ours / f"{query}.txt").read_text(encoding="utf-8"))
        theirs = psql(program, PG_DATABASE, "-A", "-t", "-F", "|", "-f", str(path.resolve()))
        if mine is None:
            verdict = "FAIL: our query failed (see ours/" + query + ".txt)"
        elif theirs.returncode != 0:
            verdict = "FAIL: PostgreSQL error: " + theirs.stderr.strip()
        else:
            pg_rows = [line.split("|") for line in theirs.stdout.splitlines() if line != ""]
            if normalise(mine, ordered) == normalise(pg_rows, ordered):
                verdict = "pass"
                passed += 1
            else:
                verdict = (f"FAIL: {len(mine)} rows (ours) vs {len(pg_rows)} (PostgreSQL)"
                           + (", ORDER BY: check ties by hand" if ordered else ""))
        lines.append(f"| {query} | {verdict} |")
    lines.append(f"\n{passed} of {len(queries)} queries match PostgreSQL.")
    text = "\n".join(lines) + "\n"
    (OUT / "compare.md").write_text(text, encoding="utf-8")
    print(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["generate", "run", "compare"])
    parser.add_argument("folder", nargs="?", type=Path)
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--bin", type=Path, default=ROOT / "build" / "apps")
    parser.add_argument("--ours", type=Path, default=OUT / "ours")
    parser.add_argument("--psql", default="psql")
    args = parser.parse_args()
    if args.command == "generate":
        generate()
    elif args.folder is None:
        parser.error(f"{args.command} needs the workload FOLDER")
    elif args.command == "run":
        run(args.folder.resolve(), args.frames, args.bin.resolve())
    else:
        compare(args.folder.resolve(), args.ours.resolve(), args.psql)


if __name__ == "__main__":
    main()
