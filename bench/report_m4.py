"""Produces the measurements of REPORT_M4.md (SPEC §M4.7, §M4.8, §M4.9).

Usage (from the repo root, after building):
    python bench/report_m4.py [build/apps]

Like bench/report_m2.py, it only calls the command-line tools:
- `storage create-table` / `storage load` build the tables from CSV files;
- `opt analyze` gathers the statistics;
- `sql compile` turns each SQL query into a logical plan (q.json);
- `opt plan` / `opt explain --json` / `opt dp-table` make and describe the physical plan;
- `exec trace --json` runs a plan and reports the actual rows and page I/O of every node.

The data is our own (the instructor will not post the M4 workload,
DECISIONS 2026-11-11). It is made by `random.Random(SEED)`, so two runs write
byte-identical files. Everything goes to bench/out/m4/ (deleted first); the
tables are printed as Markdown and also written to bench/out/m4/tables.md.
A full run takes about 30 minutes, mostly the unoptimized and rule-off plans.

Self-checks (the run stops with a message if one fails):
1. estimates and trace rows pair up one to one, and every estimate is at least
   1 row (so every q-error = larger / smaller is defined and >= 1);
2. the optimized and the unoptimized plan return the same rows for every query;
3. the correlated query's estimate is at least 10 times below the actual;
4. at least one query changes join algorithm across B = 4, 32, 256.
"""

import json
import math
import random
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "apps"
OUT = ROOT / "bench" / "out" / "m4"
DB = OUT / "db"

SEED = 3005
B_MAIN = 32           # the B of measurements 1-3
B_SMALL = 4           # measurements 2 and 3 are repeated at the smallest B
B_VALUES = [4, 32, 256]
JOINS = ["nested_loop_join", "sort_merge_join", "hash_join"]
SHORT = {"nested_loop_join": "BNL", "sort_merge_join": "SMJ", "hash_join": "Hash"}
TABLE_SIZE = {}       # table -> (rows, data pages), from `opt analyze --json`
TABLE_COLUMNS = {}    # table -> [(column, width in bytes)], the widths opt uses


# ---------------------------------------------------------------- running the tools

def tool(name, *args, check=True):
    """Runs build/apps/<name> with the arguments; returns its stdout."""
    result = subprocess.run([str(BIN / name), *map(str, args)], capture_output=True, text=True, encoding="utf-8")
    if check and result.returncode != 0:
        sys.exit(f"{name} {' '.join(map(str, args))} failed ({result.returncode}):\n{result.stderr}")
    return result.stdout


def opt(command, query, frames, *flags):
    return tool("opt", command, "--db", DB, "--logical", OUT / f"{query}.json", "--frames", frames, *flags)


def trace_plan(plan_path, frames):
    """`exec trace --json` on a physical plan file: per node (pre-order) and totals."""
    return json.loads(tool("exec", "trace", "--db", DB, "--plan", plan_path, "--frames", frames, "--json"))


def run_query(query, frames, *flags):
    """`opt plan` then `exec trace`. Returns (plan JSON, trace JSON)."""
    path = OUT / f"{query}.plan{''.join(flags).replace('-', '_')}.{frames}.json"
    path.write_text(opt("plan", query, frames, *flags), encoding="utf-8")
    return json.loads(path.read_text(encoding="utf-8")), trace_plan(path, frames)


def io(trace):
    return trace["totals"]["page_reads"] + trace["totals"]["page_writes"]


def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    lines += ["| " + " | ".join(str(v) for v in row) + " |" for row in rows]
    return "\n".join(lines)


# ---------------------------------------------------------------- 1. the data

CUST_ROWS = 32000


def make_tables():
    """name -> (schema text, rows). Every value comes from one seeded generator.

    The correlated pair is Cust.city / Cust.zip: zip is uniform in 0..999 and the
    city is fixed by the zip (zips 0-49 are in City00, 50-99 in City01, ...,
    20 cities), so knowing the zip tells the city exactly. Cust has a 60-byte
    note so that a few percent of it already fills many pages."""
    rng = random.Random(SEED)
    dept = [(d, f"dept{d:02d}", rng.randint(100, 5000)) for d in range(50)]
    emp = [(e, rng.randint(0, 49), rng.randint(20, 69), rng.randint(30000, 129999), f"emp{e:05d}")
           for e in range(10000)]
    proj = [(p, rng.randint(0, 49), f"proj{p:03d}", rng.randint(1000, 99999)) for p in range(500)]
    works = [(i // 3, rng.randint(0, 499), rng.randint(1, 40)) for i in range(30000)]
    cust = []
    for c in range(CUST_ROWS):
        zip_code = rng.randint(0, 999)
        cust.append((c, f"City{zip_code // 50:02d}", zip_code, f"cust{c:05d}", "n" * 60))
    orders = [(o, rng.randint(0, CUST_ROWS - 1), rng.randint(1, 1000), rng.choice(["open", "paid", "shipped"]))
              for o in range(3 * CUST_ROWS)]
    return {
        "Dept": ("did:INT,dname:VARCHAR(12),budget:INT", dept),
        "Emp": ("eid:INT,did:INT,age:INT,salary:INT,name:VARCHAR(16)", emp),
        "Proj": ("pid:INT,did:INT,pname:VARCHAR(12),budget:INT", proj),
        "Works": ("eid:INT,pid:INT,hours:INT", works),
        "Cust": ("cid:INT,city:VARCHAR(8),zip:INT,name:VARCHAR(16),note:VARCHAR(60)", cust),
        "Orders": ("oid:INT,cid:INT,amount:INT,status:VARCHAR(8)", orders),
        # The §M4.5 shape as real tables (section 6): 50 rows per page, so
        # A = 100 pages, Bp = 10, C = 400 (a 77-byte record + a 4-byte slot: 4088 // 81).
        "A": ("x:INT,pad:VARCHAR(71)", [(i % 2500, "a" * 71) for i in range(5000)]),
        "Bp": ("x:INT,y:INT,pad:VARCHAR(67)", [(j, j, "b" * 67) for j in range(500)]),
        "C": ("y:INT,pad:VARCHAR(71)", [(i % 5000, "c" * 71) for i in range(20000)]),
    }


# The workload: twenty queries over Dept, Emp, Proj, Works, Cust and Orders.
# Joins of three or more tables are written with JOIN ON: written as commas,
# the unoptimized plan (no pushdown) would build a cross product of millions of rows.
QUERIES = {
    "q01": ("equality", "SELECT * FROM Emp e WHERE e.did = 7"),
    "q02": ("range", "SELECT e.name, e.salary FROM Emp e WHERE e.age > 60"),
    "q03": ("AND of equality and range", "SELECT e.eid FROM Emp e WHERE e.did = 7 AND e.age > 60"),
    "q04": ("OR", "SELECT e.eid, e.age FROM Emp e WHERE e.age < 25 OR e.salary > 120000"),
    "q05": ("AND on the correlated pair",
            "SELECT c.cid, c.name FROM Cust c WHERE c.city = 'City00' AND c.zip < 50"),
    "q06": ("2-way, commas", "SELECT e.name, d.dname FROM Emp e, Dept d WHERE e.did = d.did AND d.budget > 4000"),
    "q07": ("2-way, JOIN ON", "SELECT e.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE w.hours > 35"),
    "q08": ("2-way, JOIN ON",
            "SELECT p.pname, d.dname FROM Proj p JOIN Dept d ON p.did = d.did WHERE p.budget < 10000"),
    "q09": ("2-way, commas", "SELECT w.eid, p.pname FROM Works w, Proj p WHERE w.pid = p.pid AND p.budget > 90000"),
    "q10": ("AND on one table", "SELECT o.oid, o.amount FROM Orders o WHERE o.amount > 990 AND o.status = 'paid'"),
    "q11": ("3-way", "SELECT e.name, p.pname FROM Emp e JOIN Works w ON e.eid = w.eid "
                     "JOIN Proj p ON w.pid = p.pid WHERE p.budget > 95000"),
    "q12": ("3-way", "SELECT e.name, w.hours FROM Dept d JOIN Emp e ON d.did = e.did "
                     "JOIN Works w ON e.eid = w.eid WHERE d.dname = 'dept07' AND w.hours < 5"),
    "q13": ("4-way", "SELECT e.name, p.pname, d.dname FROM Emp e JOIN Works w ON e.eid = w.eid "
                     "JOIN Proj p ON w.pid = p.pid JOIN Dept d ON p.did = d.did WHERE d.budget < 500 AND e.age > 65"),
    "q14": ("group_by + HAVING", "SELECT e.did, COUNT(*) AS n, AVG(e.salary) AS avg_sal FROM Emp e "
                                 "GROUP BY e.did HAVING COUNT(*) > 210"),
    "q15": ("join + group_by", "SELECT d.dname, COUNT(*) AS n FROM Emp e JOIN Dept d ON e.did = d.did "
                               "WHERE e.age > 50 GROUP BY d.dname"),
    "q16": ("sort + limit", "SELECT e.name, e.salary FROM Emp e WHERE e.did < 5 ORDER BY e.salary DESC LIMIT 10"),
    "q17": ("distinct", "SELECT DISTINCT p.did FROM Proj p WHERE p.budget > 50000"),
    "q18": ("union", "SELECT e.eid FROM Emp e WHERE e.age > 65 UNION SELECT w.eid FROM Works w WHERE w.hours = 40"),
    "q19": ("except", "SELECT e.eid FROM Emp e WHERE e.did = 3 EXCEPT SELECT w.eid FROM Works w WHERE w.hours > 20"),
    "q20": ("derived table", "SELECT t.did, t.n FROM (SELECT e.did, COUNT(*) AS n FROM Emp e GROUP BY e.did) AS t "
                             "WHERE t.did < 10"),
}

# Section 4 (where it breaks): the same join with both correlated conditions
# (the optimizer's estimate is far too low) and with only the zip condition,
# which implies the city one (the estimate is right).
CORRELATED = {
    "bad": ("both conditions", "SELECT * FROM Cust c, Orders o "
                               "WHERE c.cid = o.cid AND c.city = 'City00' AND c.zip < 50"),
    "zip_only": ("zip condition only", "SELECT * FROM Cust c, Orders o WHERE c.cid = o.cid AND c.zip < 50"),
}
SPEC_SHAPE = {"m45": ("§M4.5 shape", "SELECT * FROM A, Bp, C WHERE A.x = Bp.x AND Bp.y = C.y")}


def build_database(tables, queries):
    """Creates, loads and analyzes every table (sizes into TABLE_SIZE);
    compiles every query to OUT/<id>.json."""
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    for name, (schema, rows) in tables.items():
        csv = OUT / f"{name}.csv"
        csv.write_text("".join(",".join(map(str, r)) + "\n" for r in rows), encoding="utf-8")
        tool("storage", "create-table", "--db", DB, "--name", name, "--schema", schema)
        tool("storage", "load", "--db", DB, "--table", name, "--file", csv)
        summary = json.loads(tool("opt", "analyze", "--db", DB, "--table", name, "--frames", B_MAIN, "--json"))
        TABLE_SIZE[name] = (summary["rows"], summary["pages"])
        TABLE_COLUMNS[name] = column_widths(schema, summary["width"])
    for query, (_, sql) in queries.items():
        (OUT / f"{query}.sql").write_text(sql + "\n", encoding="utf-8")
        (OUT / f"{query}.json").write_text(tool("sql", "compile", "--db", DB, "--file", OUT / f"{query}.sql"),
                                           encoding="utf-8")


# ---------------------------------------------------------------- reading a trace

def subtree_end(nodes, i):
    """Index just past node i's subtree (pre-order: its subtree is the run of
    following nodes deeper than it)."""
    end = i + 1
    while end < len(nodes) and nodes[end]["depth"] > nodes[i]["depth"]:
        end += 1
    return end


def scan_passes(node):
    """How many times a seqscan read its table: rows returned / table rows."""
    rows = TABLE_SIZE[node["label"].split(" ", 1)[1]][0]
    return node["tuples_out"] / rows if rows else 1


def actual_rows(nodes):
    """Actual rows per node, counted per pass for the inner input of a nested loop.

    Exec reruns the inner input once per block of the outer and counts every
    row each time, so a 500-row inner read 3 times reports 1,500. Opt's estimate
    is for one pass (500), so each node of the inner subtree is divided by the
    number of passes, read off its seqscan (rows returned / table rows)."""
    divisor = [1.0] * len(nodes)
    for i, node in enumerate(nodes):
        if node["label"] != "nested_loop_join":
            continue
        inner = subtree_end(nodes, i + 1)        # the inner (right) input starts after the outer's subtree
        end = subtree_end(nodes, inner)
        scan = next(j for j in range(inner, end) if nodes[j]["label"].startswith("seqscan "))
        passes = scan_passes(nodes[scan])
        if passes > 0:                           # 0 passes: the outer was empty, the inner never ran
            for j in range(inner, end):
                divisor[j] *= passes
    return [round(n["tuples_out"] / d) for n, d in zip(nodes, divisor)]


def q_error(estimate, actual):
    """How many times the estimate is off. An actual 0 counts as 1 (estimates are never below 1)."""
    actual = max(1, actual)
    return max(estimate, actual) / min(estimate, actual)


def join_algorithms(plan):
    """The plan's join operators in pre-order, as short names: 'Hash, BNL'."""
    found = []

    def walk(node):
        if node["op"] in JOINS:
            found.append(SHORT[node["op"]])
        for key in ("child", "left", "right"):
            if key in node:
                walk(node[key])
    walk(plan)
    return ", ".join(found) or "no join"


def directory_reads(nodes):
    """Directory-page reads in a trace: a seqscan's reads minus its data pages
    times its passes. Opt counts data pages only (decision 2026-11-09)."""
    total = 0
    for node in nodes:
        if node["label"].startswith("seqscan "):
            pages = TABLE_SIZE[node["label"].split(" ", 1)[1]][1]
            total += node["page_reads"] - round(scan_passes(node) * pages)
    return total


# ---------------------------------------------------------------- the plan total that opt predicts

# Operators whose §M4.4 formula includes one read of each input (a sort reads
# M once, a hash join reads M + N once, ...). In exec those first reads are the
# inputs' own I/O (the scans), so the plan total takes them off once.
READS_INPUTS = set(JOINS) | {"sort", "distinct", "group_by", "union", "intersect", "except"}


def column_widths(schema, width):
    """Opt's column widths (statistics.h `columnWidths`): INT 4 bytes, the
    VARCHAR columns share the rest of the table's average width equally."""
    columns = [part.split(":") for part in schema.split(",")]
    ints = sum(1 for _, kind in columns if kind == "INT")
    varchars = len(columns) - ints
    share = (width - 4 * ints) / varchars if varchars else 4
    return [(name, 4 if kind == "INT" else share) for name, kind in columns]


def output_columns(node, children):
    """(name, width) of each output column of a physical plan node, as opt
    estimates them (estimate.h); children = the children's lists."""
    op = node["op"]
    if op == "seqscan":
        return [(f"{node['table']}.{c}", w) for c, w in TABLE_COLUMNS[node["table"]]]
    if op == "rename":
        return [(f"{node['to']}.{name.split('.')[-1]}", w) for name, w in children[0]]
    if op == "project":
        widths = dict(children[0])
        return [(name, widths[name]) for name in node["columns"]]
    if op in JOINS:
        return children[0] + children[1]
    if op == "group_by":
        widths = dict(children[0])
        out = [(key, widths[key]) for key in node["keys"]]
        for fn, column in node["aggregates"]:
            if column == "*":
                out.append(("count_star", 4))
                continue
            name = f"{fn}_{column.replace('.', '_')}"     # aggregateOutputName: avg_e_salary
            out.append((name, {"count": 4, "sum": 4, "avg": 8}.get(fn, widths.get(column, 4))))
        return out
    return children[0]                                    # filter, sort, limit, distinct, set operations


def ceil_pages(rows, width):
    """Opt's ceilPages: ceil(rows x width / 4096) with a tiny rounding guard, at least 1."""
    x = rows * width / 4096
    return max(1, math.ceil(x - 1e-9 * max(1.0, abs(x))))


def predicted_total(plan, estimates):
    """The plan's total predicted I/O: the sum of explain's per-node est_cost,
    minus one read of each input of the operators in READS_INPUTS (that read
    is already the input's own cost, e.g. its scan)."""
    flat = []                                             # (node, columns, child indices), pre-order

    def walk(node):
        index = len(flat)
        flat.append(None)
        kids = [walk(node[key]) for key in ("child", "left", "right") if key in node]
        flat[index] = (node, output_columns(node, [flat[k][1] for k in kids]), kids)
        return index
    walk(plan)
    # Estimated pages per node, as estimate.h computes them: a scan has the
    # catalog's pages, a rename or sort its child's, the rest ceil(rows x width / 4096).
    pages = [0] * len(flat)
    for i in reversed(range(len(flat))):                  # children come after their parent in pre-order
        node, columns, kids = flat[i]
        if node["op"] == "seqscan":
            pages[i] = TABLE_SIZE[node["table"]][1]
        elif node["op"] in ("rename", "sort"):
            pages[i] = pages[kids[0]]
        else:
            pages[i] = ceil_pages(estimates[i]["est_rows"], sum(w for _, w in columns))
    total = sum(e["est_cost"] for e in estimates)
    for node, _, kids in flat:
        if node["op"] in READS_INPUTS:
            total -= sum(pages[k] for k in kids)
    return total


# ---------------------------------------------------------------- the workload at B = 32

def run_workload():
    """Measurements 1 and 2 and the predicted-vs-actual table, one pass over the twenty queries.

    Per query: the estimates (`opt explain --json`), the optimized plan's trace
    and the unoptimized plan's trace. Returns one dict per query."""
    results = {}
    for query in QUERIES:
        estimates = json.loads(opt("explain", query, B_MAIN, "--json"))
        plan, trace = run_query(query, B_MAIN)
        nodes = trace["nodes"]
        if len(estimates) != len(nodes):                                       # self-check 1
            sys.exit(f"{query}: {len(estimates)} estimates but {len(nodes)} trace rows")
        if min(e["est_rows"] for e in estimates) < 1:                          # self-check 1
            sys.exit(f"{query}: an estimate below 1 row (§M4.2: never below 1)")
        actual = actual_rows(nodes)
        errors = [q_error(e["est_rows"], a) for e, a in zip(estimates, actual)]
        measured = {}
        for frames in (B_MAIN, B_SMALL):
            _, optimized = run_query(query, frames)
            _, unoptimized = run_query(query, frames, "--unoptimized")
            for t in (optimized, unoptimized):                                 # self-check 2
                if t["nodes"][0]["tuples_out"] != nodes[0]["tuples_out"]:
                    sys.exit(f"{query} at B = {frames}: optimized and unoptimized row counts differ")
            measured[frames] = (io(optimized), io(unoptimized))
        results[query] = {"plan": plan, "nodes": nodes, "estimates": estimates, "actual": actual,
                          "errors": errors, "under_limit": under_limit(nodes), "measured": measured,
                          "io": measured[B_MAIN][0], "rows": nodes[0]["tuples_out"]}
    return results


def under_limit(nodes):
    """True for each node below a limit: it only returns the rows the limit
    pulls (a sort of 1,000 rows under `limit 10` reports 10), so its actual
    count says nothing about the estimate."""
    flags = [False] * len(nodes)
    for i, node in enumerate(nodes):
        if node["label"] == "limit":
            for j in range(i + 1, subtree_end(nodes, i)):
                flags[j] = True
    return flags


def q_error_tables(results):
    per_query, every_node, all_errors, counted = [], [], [], []
    for query, r in results.items():
        worst = max(range(len(r["errors"])), key=lambda i: r["errors"][i])
        per_query.append([query, len(r["errors"]), f"{statistics.median(r['errors']):.2f}",
                          f"{r['errors'][worst]:.2f}", r["nodes"][worst]["label"],
                          r["estimates"][worst]["est_rows"], r["actual"][worst]])
        for i, node in enumerate(r["nodes"]):
            every_node.append([query, i, "  " * node["depth"] + node["label"], r["estimates"][i]["est_rows"],
                               r["actual"][i], f"{r['errors'][i]:.2f}", "yes" if r["under_limit"][i] else ""])
            if not r["under_limit"][i]:
                counted.append(r["errors"][i])
        all_errors += r["errors"]
    summary = {"nodes": len(all_errors), "median": statistics.median(all_errors), "worst": max(all_errors),
               "exact": sum(1 for e in all_errors if e == 1), "within2": sum(1 for e in all_errors if e <= 2),
               "over10": sum(1 for e in all_errors if e > 10), "counted": len(counted),
               "median_counted": statistics.median(counted), "worst_counted": max(counted)}
    return (table(["Query", "Nodes", "Median q-error", "Worst q-error", "Worst node", "Est. rows", "Actual rows"],
                  per_query),
            table(["Query", "#", "Node", "Est. rows", "Actual rows", "q-error", "Under a limit"], every_node),
            summary)


def pays_table(results):
    rows, ratios = [], {B_MAIN: [], B_SMALL: []}
    for query, r in results.items():
        row = [query, QUERIES[query][0], r["rows"]]
        for frames in (B_MAIN, B_SMALL):
            optimized, unoptimized = r["measured"][frames]
            ratios[frames].append(unoptimized / optimized)
            row += [optimized, unoptimized, f"{unoptimized / optimized:.2f}"]
        rows.append(row)
    headers = ["Query", "Kind", "Rows"]
    for frames in (B_MAIN, B_SMALL):
        headers += [f"Optimized I/O (B = {frames})", f"Unoptimized I/O (B = {frames})", f"Ratio (B = {frames})"]
    ranges = {frames: (min(v), max(v)) for frames, v in ratios.items()}
    return table(headers, rows), ranges


def predicted_table(frames):
    """Opt's predicted plan total against exec's measured page I/O, every query at B = frames."""
    rows = []
    for query in QUERIES:
        estimates = json.loads(opt("explain", query, frames, "--json"))
        plan, trace = run_query(query, frames)
        predicted = predicted_total(plan, estimates)
        directory = directory_reads(trace["nodes"])
        rows.append([query, join_algorithms(plan), predicted, io(trace), directory, io(trace) - directory,
                     io(trace) - directory - predicted])
    return table(["Query", "Joins", "Predicted", "Measured I/O", "Directory reads",
                  "Measured − directory", "Remaining gap"], rows)


# ---------------------------------------------------------------- 3. which rule mattered

def ablation_table(results, frames):
    """Rules 1..7 turned off one at a time, the whole workload at B = frames."""
    total = sum(r["measured"][frames][0] for r in results.values())
    rows = []
    for rule in range(1, 8):
        io_without, changed = 0, []
        for query, r in results.items():
            _, trace = run_query(query, frames, "--disable-rule", str(rule))
            if trace["nodes"][0]["tuples_out"] != r["rows"]:
                sys.exit(f"{query} without rule {rule}: a different row count")
            io_without += io(trace)
            if io(trace) != r["measured"][frames][0]:
                changed.append(f"{query} ({r['measured'][frames][0]} → {io(trace)})")
        rows.append([rule, total, io_without, io_without - total, ", ".join(changed) or "none"])
    return table(["Rule off", "Total I/O, all rules", "Total I/O without it", "Difference",
                  "Queries whose I/O changed (with → without)"], rows)


# ---------------------------------------------------------------- B changes the algorithm

def b_table():
    rows, changed = [], []
    for query in QUERIES:
        chosen = [join_algorithms(json.loads(opt("plan", query, frames))) for frames in B_VALUES]
        if chosen[0] == "no join":
            continue
        differs = len(set(chosen)) > 1
        if differs:
            changed.append(query)
        rows.append([query, *chosen, "yes" if differs else ""])
    if not changed:                                                            # self-check 4
        sys.exit("no query changes join algorithm across B")
    return table(["Query", *[f"B = {b}" for b in B_VALUES], "Changes"], rows), changed


# ---------------------------------------------------------------- 4. where it breaks

def set_condition(node, old, new):
    """Replaces a filter's condition `old` by `new` everywhere in a plan."""
    if node["op"] == "filter" and node["condition"] == old:
        node["condition"] = new
    for key in ("child", "left", "right"):
        if key in node:
            set_condition(node[key], old, new)


def where_it_breaks():
    """The correlated query: estimate vs actual, then the bad plan against the better one at several B."""
    estimates = json.loads(opt("explain", "bad", 4, "--json"))
    _, trace = run_query("bad", 4)
    filter_index = next(i for i, n in enumerate(trace["nodes"]) if n["label"] == "filter")
    estimate, actual = estimates[filter_index]["est_rows"], trace["nodes"][filter_index]["tuples_out"]
    if estimate * 10 > actual:                                                 # self-check 3
        sys.exit(f"the correlated filter is estimated {estimate} rows, actual {actual}: not 10 times off")
    rows = []
    for frames in [4, 6, 8, 12, 16, 32]:
        bad_plan, bad = run_query("bad", frames)
        better_plan, _ = run_query("zip_only", frames)
        # The better plan for the real query: the zip-only plan with both conditions back in its filter.
        set_condition(better_plan, "c.zip < 50", "c.city = 'City00' and c.zip < 50")
        path = OUT / f"better.{frames}.json"
        path.write_text(json.dumps(better_plan, indent=2), encoding="utf-8")
        better = trace_plan(path, frames)
        if better["nodes"][0]["tuples_out"] != bad["nodes"][0]["tuples_out"]:
            sys.exit(f"B = {frames}: the better plan returns a different row count")
        rows.append([frames, join_algorithms(bad_plan), io(bad), join_algorithms(better_plan), io(better),
                     f"{io(bad) / io(better):.2f}"])
    text = {"explain_bad": opt("explain", "bad", 4), "explain_zip": opt("explain", "zip_only", 4),
            "dp_bad": opt("dp-table", "bad", 4), "dp_zip": opt("dp-table", "zip_only", 4)}
    return estimate, actual, table(["B", "Plan chosen (both conditions)", "Its I/O",
                                    "Plan chosen with the right estimate", "Its I/O", "Bad / better"], rows), text


# ---------------------------------------------------------------- 6. the §M4.5 shape

def spec_shape():
    """The §M4.5 example as real tables at B = 32: the DP's order against the other one."""
    plan, chosen = run_query("m45", B_MAIN)
    # The other order, (Bp ⋈ A) ⋈ C, written by hand with the DP's top project.
    other = {"op": "project", "columns": plan["columns"], "child": {
        "op": "nested_loop_join", "condition": "Bp.y = C.y",
        "left": {"op": "nested_loop_join", "condition": "A.x = Bp.x",
                 "left": {"op": "seqscan", "table": "Bp"}, "right": {"op": "seqscan", "table": "A"}},
        "right": {"op": "seqscan", "table": "C"}}} if plan["op"] == "project" else None
    if other is None:
        sys.exit("the §M4.5 plan has no top project; update spec_shape()")
    path = OUT / "m45.other.json"
    path.write_text(json.dumps(other, indent=2), encoding="utf-8")
    other_trace = trace_plan(path, B_MAIN)
    if other_trace["nodes"][0]["tuples_out"] != chosen["nodes"][0]["tuples_out"]:
        sys.exit("the two §M4.5 orders return different row counts")
    lines = [[n["label"], n["tuples_out"], n["page_reads"], n["page_writes"]] for n in chosen["nodes"]]
    other_lines = [[n["label"], n["tuples_out"], n["page_reads"], n["page_writes"]] for n in other_trace["nodes"]]
    return {"dp": opt("dp-table", "m45", B_MAIN), "explain": opt("explain", "m45", B_MAIN),
            "chosen_io": io(chosen), "other_io": io(other_trace),
            "chosen": table(["Node", "Rows out", "Reads", "Writes"], lines),
            "other": table(["Node", "Rows out", "Reads", "Writes"], other_lines)}


# ---------------------------------------------------------------- main

def main():
    sys.stdout.reconfigure(encoding="utf-8")
    build_database(make_tables(), {**QUERIES, **CORRELATED, **SPEC_SHAPE})
    sizes = table(["Table", "Rows", "Data pages"], [[t, r, p] for t, (r, p) in TABLE_SIZE.items()])
    results = run_workload()
    per_query, every_node, summary = q_error_tables(results)
    pays, ranges = pays_table(results)
    by_b, changed = b_table()
    estimate, actual, breaks, breaks_text = where_it_breaks()
    shape = spec_shape()
    sections = [
        ("Tables", sizes),
        ("1. Estimation quality (B = 32)",
         f"{summary['nodes']} nodes; median q-error {summary['median']:.2f}; worst {summary['worst']:.2f}; "
         f"exact {summary['exact']}; within 2x {summary['within2']}; over 10x {summary['over10']}; "
         f"without the {summary['nodes'] - summary['counted']} nodes under a limit: median "
         f"{summary['median_counted']:.2f}, worst {summary['worst_counted']:.2f}\n\n{per_query}"),
        ("1b. Every node", every_node),
        ("2. Does optimization pay",
         "; ".join(f"B = {b}: ratio range {lo:.2f} .. {hi:.2f}" for b, (lo, hi) in ranges.items()) + f"\n\n{pays}"),
        (f"3. Which rule mattered (B = {B_MAIN})", ablation_table(results, B_MAIN)),
        (f"3b. Which rule mattered (B = {B_SMALL})", ablation_table(results, B_SMALL)),
        ("4. Where it breaks", f"filter estimate {estimate} rows, actual {actual} rows "
                               f"({actual / estimate:.1f} times more)\n\n{breaks}\n\n```\n{breaks_text['explain_bad']}```"
                               f"\n\n```\n{breaks_text['explain_zip']}```\n\n{breaks_text['dp_bad']}\n\n"
                               f"{breaks_text['dp_zip']}"),
        (f"5. Predicted cost against measured I/O (B = {B_MAIN})", predicted_table(B_MAIN)),
        (f"5b. Predicted cost against measured I/O (B = {B_SMALL})", predicted_table(B_SMALL)),
        ("6. The §M4.5 shape as real tables (B = 32)",
         f"DP's order: {shape['chosen_io']} page I/O; other order: {shape['other_io']}\n\n{shape['dp']}\n\n"
         f"```\n{shape['explain']}```\n\nDP's order, measured:\n\n{shape['chosen']}\n\n"
         f"Other order, measured:\n\n{shape['other']}"),
        ("7. B changes the join algorithm", f"changes: {', '.join(changed)}\n\n{by_b}"),
        ("8. Four-relation DP table (q13, B = 32)", opt("dp-table", "q13", B_MAIN)),
    ]
    text = "\n\n".join(f"## {title}\n\n{body}" for title, body in sections) + "\n"
    (OUT / "tables.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
