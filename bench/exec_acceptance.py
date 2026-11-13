"""Checks the `exec` tool against modules/exec/SPEC.md, one spec line at a time.

Usage (from the repo root, after building):
    python bench/exec_acceptance.py [path/to/exec.exe] [path/to/storage.exe]

Black box: it only runs the real `storage` and `exec` programs, as a grader
would. It builds its own tables with `storage create-table` / `storage load`
(the instructor will not post datasets or plans; the inputs are our own), works out the expected
rows itself in plain Python (independent of the C++ code), and compares:

- result rows, as a bag (§M2.5 "Output is checked for correct tuples, as a bag");
- counters against the spec's formulas (§M2.3, §M2.5);
- the memory rule: every trace reports max_frames_held <= B (§M2.1);
- no temporary file left in the database after any run (§M2.1);
- errors: exit code and message (§M2.2, §M2.1);
- the output formats (§M2.5).

Every check has a verdict:
    PASS     the spec line holds exactly.
    ALLOWED  measured differs from a formula, and the spec allows it: the
             quoted line asks us to state or explain the difference (it goes
             into DESIGN_M2.md / REPORT_M2.md).
    FAIL     a strict spec line does not hold: a bug.
The full table is written to bench/out/exec/acceptance.md. Exit code 1 if any
check FAILs. Everything it creates is in bench/out/exec/, deleted first.

The same table builders and runners are meant for REPORT_M2.md (chunk 16).
"""

import json
import math
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "bench" / "out" / "exec"
DB = OUT / "db"
EXEC = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "apps" / "exec.exe")
STORAGE = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "build" / "apps" / "storage.exe")

ROWS_PER_PAGE = 240  # R(a INT, b VARCHAR(10), c INT) with 3-byte b: 4088 // (13 + 4)

# ---------------------------------------------------------------- results

RESULTS = []  # (id, spec quote, what was checked, verdict, evidence)


def record(req, quote, what, verdict, evidence=""):
    RESULTS.append((req, quote, what, verdict, str(evidence)))
    mark = {"PASS": ".", "ALLOWED": "a", "FAIL": "F"}[verdict]
    print(mark, end="", flush=True)


def check(req, quote, what, ok, evidence=""):
    record(req, quote, what, "PASS" if ok else "FAIL", evidence)


def formula(req, quote, what, measured, expected, allowed_quote, explanation):
    """A counter against a formula: PASS if equal; else ALLOWED, citing the
    line that asks us to explain the gap, with the explanation."""
    if measured == expected:
        record(req, quote, what, "PASS", f"measured {measured} = formula {expected}")
    else:
        record(req, quote, what, "ALLOWED",
               f"measured {measured}, formula {expected}. {explanation} Spec: {allowed_quote}")


GAP = "§M2.6: \"Where measurement and formula disagree, explain the gap\""

# ---------------------------------------------------------------- tables

TABLES = {}  # name -> (list of column (name, type), rows)


def load_table(name, columns, rows):
    """columns: [("a", "INT"), ("b", "VARCHAR(10)")]."""
    schema = ",".join(f"{c}:{t}" for c, t in columns)
    csv = OUT / f"{name}.csv"
    csv.write_text("".join(",".join(str(v) for v in r) + "\n" for r in rows), encoding="utf-8")
    for args in (["create-table", "--db", DB, "--name", name, "--schema", schema],
                 ["load", "--db", DB, "--table", name, "--file", csv]):
        result = subprocess.run([STORAGE, *map(str, args)], capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f"storage {args[0]} {name} failed: {result.stderr}")
    TABLES[name] = (columns, [tuple(r) for r in rows])


def r_rows(n):
    """Row i: a = i, b = 'csc' / 'eng', c = i mod 7. 240 rows per page."""
    return [(i, "csc" if i % 2 == 0 else "eng", i % 7) for i in range(n)]


R_COLUMNS = [("a", "INT"), ("b", "VARCHAR(10)"), ("c", "INT")]
K_COLUMNS = [("a", "INT"), ("b", "INT")]


def k_rows(bs):
    return [(i, b) for i, b in enumerate(bs)]


def pages(name):
    return math.ceil(len(TABLES[name][1]) / ROWS_PER_PAGE)


# ---------------------------------------------------------------- running exec


def scan(t):
    return {"op": "seqscan", "table": t}


def node(op, **fields):
    return {"op": op, **fields}


def exec_cmd(command, plan, frames, json_flag=True, plan_text=None):
    """Runs `exec command` on `plan`; returns (exit code, stdout, stderr)."""
    path = OUT / "plan.json"
    path.write_text(plan_text if plan_text is not None else json.dumps(plan), encoding="utf-8")
    args = [EXEC, command, "--db", str(DB), "--plan", str(path), "--frames", str(frames)]
    if json_flag:
        args.append("--json")
    result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    return result.returncode, result.stdout, result.stderr


def temp_files_left():
    tmp = DB / "tmp"
    return sorted(p.name for p in tmp.iterdir()) if tmp.exists() else []


def run_plan(plan, frames):
    """`exec run --json` and `exec trace --json` on the same plan. Returns
    (rows as a Counter of tuples, run JSON, trace JSON). Also checks the
    rules every run must keep: exit 0, frames <= B, no temp file left."""
    code, out, err = exec_cmd("run", plan, frames)
    if code != 0:
        raise RuntimeError(f"exec run failed ({code}): {err}\nplan: {json.dumps(plan)}")
    result = json.loads(out)
    code, out, err = exec_cmd("trace", plan, frames)
    if code != 0:
        raise RuntimeError(f"exec trace failed ({code}): {err}")
    trace = json.loads(out)
    MEMORY.append((json.dumps(plan)[:80], frames, trace["max_frames_held"], temp_files_left()))
    return Counter(tuple(r) for r in result["rows"]), result, trace


MEMORY = []  # (plan, B, max frames held, temp files left) for every run


def bag(rows):
    return Counter(tuple(r) for r in rows)


def own(trace, i=0):
    return trace["nodes"][i]


def sub(trace, i=0):
    return trace["nodes"][i]["subtree"]


# ---------------------------------------------------------------- Python model


def rows_of(t):
    return TABLES[t][1]


def col(t, name):
    return [c for c, _ in TABLES[t][0]].index(name)


def join_rows(left, right, test):
    return [l + r for l in rows_of(left) for r in rows_of(right) if test(l, r)]


# ---------------------------------------------------------------- the checks


def check_scan_filter_project():
    q = "§M2.3 seqscan: \"Every live tuple, heap order, one data page pinned at a time. Frames 1. Cost M reads. Comparisons 0.\""
    for frames in (4, 16):
        rows, run, trace = run_plan(scan("R10"), frames)
        check("M2.3-scan-1", q, f"seqscan R10 returns every tuple, in heap order (B={frames})",
              run["rows"] == [list(r) for r in rows_of("R10")])
        check("M2.3-scan-2", q, f"seqscan comparisons 0, writes 0 (B={frames})",
              own(trace)["comparisons"] == 0 and own(trace)["page_writes"] == 0)
        check("M2.3-scan-3", q, f"seqscan holds 1 frame (B={frames})", trace["max_frames_held"] == 1,
              f"max_frames_held {trace['max_frames_held']}")
        formula("M2.3-scan-4", q, f"seqscan reads = M (M = 10, B={frames})", own(trace)["page_reads"], 10, GAP,
                "The scan also reads the table's directory page once (§M1.5 heap file layout).")

    qf = "§M2.3 filter: \"Output schema = input schema. Pipelined, frames 1, no I/O of its own. Comparisons increase by one per tuple examined, not per comparison operator inside a compound condition.\""
    plan = node("filter", condition="R10.a > 500 and R10.b = 'csc'", child=scan("R10"))
    rows, run, trace = run_plan(plan, 4)
    expected = bag(r for r in rows_of("R10") if r[0] > 500 and r[1] == "csc")
    check("M2.3-filter-1", qf, "filter returns exactly the matching tuples", rows == expected)
    check("M2.3-filter-2", qf, "filter: 1 comparison per tuple examined, compound condition counts once",
          own(trace)["comparisons"] == len(rows_of("R10")), own(trace)["comparisons"])
    check("M2.3-filter-3", qf, "filter has no I/O of its own",
          own(trace)["page_reads"] == 0 and own(trace)["page_writes"] == 0)
    check("M2.3-filter-4", qf, "filter output schema = input schema",
          run["schema"] == [["R10.a", "INT"], ["R10.b", "VARCHAR", 10], ["R10.c", "INT"]], run["schema"])

    qp = "§M2.3 project: \"Keeps the listed columns in the listed order. It does not remove duplicates.\""
    rows, run, trace = run_plan(node("project", columns=["R10.c", "R10.b"], child=scan("R10")), 4)
    check("M2.3-project-1", qp, "project keeps the listed columns in the listed order, with duplicates",
          rows == bag((r[2], r[1]) for r in rows_of("R10")) and run["schema"][0][0] == "R10.c")
    check("M2.3-project-2", qp, "project: 2400 rows out of 14 distinct values (duplicates kept)",
          run["totals"]["tuples_out"] == 2400)


def check_sort():
    q = "§M2.3 sort: \"Must sort an input of 20 × B pages; cannot hold input in memory\""
    plan = node("sort", keys=[["P80.a", "desc"]], child=scan("P80"))
    rows, run, trace = run_plan(plan, 4)
    check("M2.3-sort-1", q, "sort of 80 pages = 20 × B at B = 4 completes, in order",
          [r[0] for r in run["rows"]] == sorted((r[0] for r in rows_of("P80")), reverse=True))
    check("M2.8-5", "§M2.6 measurement 5 / §M2.1 \"Holding more than B pages at any moment is a correctness failure\"",
          "sort of 20 × B pages at B = 4: frames held <= 4", trace["max_frames_held"] <= 4,
          f"max_frames_held {trace['max_frames_held']}")

    qk = "§M2.3 sort keys: [[\"R.a\",\"asc\"],[\"R.b\",\"desc\"]]"
    plan = node("sort", keys=[["R10.c", "asc"], ["R10.a", "desc"]], child=scan("R10"))
    rows, run, trace = run_plan(plan, 4)
    expected = sorted(rows_of("R10"), key=lambda r: (r[2], -r[0]))
    check("M2.3-sort-2", qk, "two keys, asc then desc", [tuple(r) for r in run["rows"]] == expected)

    qu = "§M2.1: \"VARCHAR ordering is byte-wise on the UTF-8 encoding.\""
    rows, run, trace = run_plan(node("sort", keys=[["U.s", "asc"]], child=scan("U")), 4)
    expected = sorted((r[0] for r in rows_of("U")), key=lambda s: s.encode("utf-8"))
    check("M2.1-utf8", qu, "VARCHAR sorts byte-wise on UTF-8 ('B' < 'Z' < 'a' < 'z' < 'é')",
          [r[0] for r in run["rows"]] == expected, [r[0] for r in run["rows"]])

    qp = "§M2.3: \"Number of passes = 1 + ceil(log(ceil(N/B)) / log(B-1))\"; §M2.6 measurement 2: \"Report both.\""
    for table, frames in (("P8", 4), ("P12", 4), ("P80", 4), ("P30", 8), ("P200", 16), ("P200", 64)):
        n = pages(table)
        rows, run, trace = run_plan(node("sort", keys=[[f"{table}.a", "asc"]], child=scan(table)), frames)
        passes = own(trace)["details"]["passes"]
        runs = math.ceil(n / frames)
        expected = 1 + (math.ceil(math.log(runs) / math.log(frames - 1) - 1e-12) if runs > 1 else 0)
        formula("M2.3-sort-passes", qp, f"sort passes, N = {n} pages, B = {frames}", passes, expected,
                "§M2.6 measurement 2: \"Report both.\"",
                "Inside a plan the input scan holds 1 frame, so pass 0 makes runs of B − 1 pages, not B.")
        # Rewrite variant (DECISIONS 2026-11-06): every pass but the last
        # writes N pages, every pass after pass 0 reads N. Exact, own I/O.
        written = own(trace)["page_writes"]
        check("M2.3-sort-io", "§M2.3: \"State which you do; measured totals must match your own description.\"",
              f"sort I/O matches the rewrite variant: own reads = own writes = N·(passes−1) (N = {n}, B = {frames})",
              written == own(trace)["page_reads"] == n * (passes - 1) or (passes == 1 and written == 0),
              f"reads {own(trace)['page_reads']}, writes {written}, passes {passes}")


def check_distinct_group_by():
    q = "§M2.3 distinct: \"Two implementations selected by the plan: sort-based ... and hash-based\""
    expected = bag(set((r[1],) for r in rows_of("D30")))
    for mode in ("sort", "hash"):
        for frames in (4, 16):
            plan = node("distinct", mode=mode, child=node("project", columns=["D30.b"], child=scan("D30")))
            rows, run, trace = run_plan(plan, frames)
            check("M2.3-distinct", q, f"distinct {mode} mode on 30 pages, B = {frames}", rows == expected,
                  f"{sum(rows.values())} rows")

    qg = "§M2.3 group_by: \"Sort-based and hash-based implementations, selected by the plan; same output as bags\""
    aggs = [["count", "*"], ["sum", "G.salary"], ["avg", "G.salary"], ["min", "G.name"], ["max", "G.name"]]
    groups = {}
    for dept, salary, name in rows_of("G"):
        groups.setdefault(dept, []).append((salary, name))
    expected = bag((d, len(v), sum(s for s, _ in v), round(sum(s for s, _ in v) / len(v), 4),
                    min((n for _, n in v), key=lambda x: x.encode()), max((n for _, n in v), key=lambda x: x.encode()))
                   for d, v in groups.items())
    for mode in ("sort", "hash"):
        plan = node("group_by", mode=mode, keys=["G.dept"], aggregates=aggs, child=scan("G"))
        rows, run, trace = run_plan(plan, 4)
        got = bag((r[0], r[1], r[2], round(r[3], 4), r[4], r[5]) for r in run["rows"])
        check("M2.3-group-1", qg, f"group_by {mode}: COUNT, SUM, AVG, MIN, MAX per group", got == expected)
        check("M2.3-group-names", "§M2.3: \"output schema: R.dept, count_star, avg_R_salary, max_R_name\"",
              f"group_by {mode} output names",
              [c[0] for c in run["schema"]] == ["G.dept", "count_star", "sum_G_salary", "avg_G_salary",
                                                 "min_G_name", "max_G_name"], run["schema"])
    code, out, err = exec_cmd("run", node("group_by", mode="hash", keys=[], aggregates=[["avg", "G.salary"]],
                                          child=scan("G")), 4, json_flag=False)
    avg = sum(r[1] for r in rows_of("G")) / len(rows_of("G"))
    check("M2.3-avg", "§M2.3: \"AVG returns a real number printed to four decimal places.\"",
          "AVG printed with 4 decimals", out.splitlines()[1] == f"{avg:.4f}", out.splitlines()[1])

    qe = "§M2.3: \"no grouping columns + empty input → exactly one row, COUNT 0, other aggregates undefined (an error if requested). With grouping columns + empty input → zero rows.\""
    for mode in ("sort", "hash"):
        rows, run, _ = run_plan(node("group_by", mode=mode, keys=[], aggregates=[["count", "*"]], child=scan("E")), 4)
        check("M2.3-group-empty-1", qe, f"{mode}: no keys + empty input → one row, COUNT 0", run["rows"] == [[0]])
        rows, run, _ = run_plan(node("group_by", mode=mode, keys=["E.a"], aggregates=[["count", "*"]],
                                     child=scan("E")), 4)
        check("M2.3-group-empty-2", qe, f"{mode}: keys + empty input → zero rows", run["rows"] == [])
        code, out, err = exec_cmd("run", node("group_by", mode=mode, keys=[], aggregates=[["sum", "E.a"]],
                                              child=scan("E")), 4)
        check("M2.3-group-empty-3", qe, f"{mode}: no keys + empty input + SUM → an error", code == 1, err.strip())
    code, out, err = exec_cmd("run", node("group_by", mode="sort", keys=[], aggregates=[["sum", "G.name"]],
                                          child=scan("G")), 4)
    check("M2.3-group-type", "§M2.3: \"SUM and AVG require INT.\"", "SUM of a VARCHAR → error",
          code == 1 and "out" not in out, err.strip())


def check_joins():
    qn = "§M2.3 nested_loop_join: \"comparisons = |R| * |S| ... does not depend on B, on the condition, or on how many pairs match\""
    for cond, test in (("R10.a = S4.a", lambda l, r: l[0] == r[0]), ("R10.a < S4.a", lambda l, r: l[0] < r[0])):
        for frames in (4, 16, 64):
            plan = node("nested_loop_join", condition=cond, left=scan("R10"), right=scan("S4"))
            rows, run, trace = run_plan(plan, frames)
            check("M2.3-nlj-rows", "§M2.3: \"Accepts any condition (not only equality)\"",
                  f"nested_loop_join {cond} rows (B = {frames})", rows == bag(join_rows("R10", "S4", test)))
            check("M2.8-4", qn, f"nested_loop_join {cond}: comparisons exactly |R|×|S| = 2400×960 (B = {frames})",
                  own(trace)["comparisons"] == 2400 * 960, own(trace)["comparisons"])

    qc = "§M2.3 nested_loop_join: \"cost = M + ceil(M / (B-2)) * N page reads\""
    qh = "§M2.3 hash_join: \"choose P = B - 1 ... cost = 2(M + N) partitioning + (M + N) probing = 3(M + N)\""
    qs = "§M2.3 sort_merge_join: \"cost = sort(M) + sort(N) + M + N\""
    for left, right in (("R10", "S4"), ("R30", "S10"), ("R60", "S20")):
        m, n = pages(left), pages(right)
        cond = f"{left}.a = {right}.a"
        expected = bag(join_rows(left, right, lambda l, r: l[0] == r[0]))
        bags = {}
        for frames in (4, 16, 64, 256):
            for op in ("nested_loop_join", "sort_merge_join", "hash_join"):
                rows, run, trace = run_plan(node(op, condition=cond, left=scan(left), right=scan(right)), frames)
                bags[op] = rows
                check("M2.8-3", "§M2.8: \"All three joins on the same inputs return the same bag of tuples.\"",
                      f"{op} {left}⋈{right} B = {frames}: correct bag", rows == expected)
                reads, writes = sub(trace)["page_reads"], sub(trace)["page_writes"]
                if op == "nested_loop_join":
                    blocks = math.ceil(m / (frames - 2))
                    formula("M2.3-nlj-cost", qc, f"NLJ reads, M = {m}, N = {n}, B = {frames}", reads,
                            m + blocks * n, GAP,
                            f"Exactly 1 + {blocks} directory-page reads more (1 per scan: the outer once, the inner once per block).")
                    # Never more than formula + directory pages; fewer only by
                    # buffer hits (a smaller last block leaves frames free).
                    exact = m + blocks * n + 1 + blocks
                    if reads == exact and writes == 0:
                        record("M2.3-nlj-cost-exact", qc, f"NLJ reads = formula + directory pages (M = {m}, B = {frames})",
                               "PASS", f"reads {reads}")
                    elif reads < exact and writes == 0:
                        record("M2.3-nlj-cost-exact", qc, f"NLJ reads = formula + directory pages (M = {m}, B = {frames})",
                               "ALLOWED", f"reads {reads}, formula + directory pages {exact}: the last block is "
                               f"smaller, so free frames keep {exact - reads} inner page(s) cached between blocks. {GAP}")
                    else:
                        record("M2.3-nlj-cost-exact", qc, f"NLJ reads = formula + directory pages (M = {m}, B = {frames})",
                               "FAIL", f"reads {reads}, writes {writes}, expected at most {exact} reads and 0 writes")
                elif op == "hash_join":
                    d = own(trace)["details"]
                    check("M2.3-hj-P", qh, f"hash_join P = B − 1 (B = {frames})", d["partitions"] == frames - 1,
                          d["partitions"])
                    formula("M2.3-hj-cost", qh, f"hash_join I/O, M = {m}, N = {n}, B = {frames}", reads + writes,
                            3 * (m + n), GAP,
                            f"Partitions written: {sum(d['left_pages']) + sum(d['right_pages'])} pages (each partition's last page is partly full; P = {frames - 1}); overflowed partitions {d['overflow_partitions']} were joined by block nested loop; +2 directory pages.")
                else:
                    def sort_cost(p):
                        runs = math.ceil(p / frames)
                        passes = 1 + (math.ceil(math.log(runs) / math.log(frames - 1) - 1e-12) if runs > 1 else 0)
                        return 2 * p * (passes - 1) + p
                    formula("M2.3-smj-cost", qs, f"sort_merge_join I/O, M = {m}, N = {n}, B = {frames}", reads + writes,
                            sort_cost(m) + sort_cost(n) + m + n, GAP,
                            "Both sorts stream their final merge into the join (\"using your sort operator\"), so the merge's M + N reads are the sorts' last pass; the two final merges share B (a sort may need one more pass, a small input is written once); pass-0 runs are B − 1 pages; the merge stops when one input ends; +2 directory pages.")

    qd = "§M2.8: \"The duplicate key example (R.b=[1,2,2,3], S.b=[2,2,4]) returns four rows.\""
    for op in ("sort_merge_join", "hash_join", "nested_loop_join"):
        rows, run, trace = run_plan(node(op, condition="KR.b = KS.b", left=scan("KR"), right=scan("KS")), 4)
        check("M2.8-2", qd, f"{op}: the duplicate-key example gives 4 rows", sum(rows.values()) == 4,
              sorted(rows.elements()))

    qo = "§M2.8: \"A join whose output is larger than both inputs completes.\""
    expected = bag(join_rows("DR", "DS", lambda l, r: l[1] == r[1]))
    for op in ("nested_loop_join", "sort_merge_join", "hash_join"):
        rows, run, trace = run_plan(node(op, condition="DR.b = DS.b", left=scan("DR"), right=scan("DS")), 4)
        check("M2.8-6", qo, f"{op}: duplicates on both sides, {sum(expected.values())} rows from 400 and 300",
              rows == expected)

    ql = "§M2.3 sort_merge_join: \"A run of equal keys may be larger than B ... assuming it fits is not [acceptable]\""
    rows, run, trace = run_plan(node("sort_merge_join", condition="LR.b = LS.b", left=scan("LR"), right=scan("LS")), 4)
    check("M2.3-smj-long", ql, "a run of 1200 × 1200 equal keys (5 pages each side) at B = 4",
          sum(rows.values()) == 1200 * 1200 + 300, sum(rows.values()))

    qk = "§M2.3 hash_join: \"Must detect a partition that still exceeds B and do something defensible.\""
    rows, run, trace = run_plan(node("hash_join", condition="SKR.b = SKS.b", left=scan("SKR"), right=scan("SKS")), 4)
    d = own(trace)["details"]
    check("M2.3-hj-skew", qk, "one key in 40 % of rows: the overflowing partition is detected, result correct",
          sum(rows.values()) == 2000 * 1000 + 1500 and len(d["overflow_partitions"]) >= 1,
          f"partitions {d['left_pages']}, overflow {d['overflow_partitions']}, strategy {d['overflow_strategy']}")

    qe = "§M2.3: \"Equality conditions only.\" (sort_merge_join, hash_join)"
    for op in ("sort_merge_join", "hash_join"):
        code, out, err = exec_cmd("run", node(op, condition="R10.a < S4.a", left=scan("R10"), right=scan("S4")), 4)
        check("M2.3-equality", qe, f"{op} with R.a < S.a is rejected", code == 1 and f"Node '{op}'" in err,
              err.strip())

    qs = "§M2.1: \"Join output schema = left schema followed by right schema, unchanged. Two columns with the same qualified name in one schema is an error at open.\""
    rows, run, trace = run_plan(node("hash_join", condition="KR.b = KS.b", left=scan("KR"), right=scan("KS")), 4)
    check("M2.1-join-schema", qs, "join output schema = left then right",
          [c[0] for c in run["schema"]] == ["KR.a", "KR.b", "KS.a", "KS.b"], run["schema"])
    for op in ("nested_loop_join", "sort_merge_join", "hash_join"):
        code, out, err = exec_cmd("run", node(op, condition="KR.b = KR.a", left=scan("KR"), right=scan("KR")), 4)
        check("M2.1-dup-names", qs, f"{op} of KR with KR (same qualified names) is an error", code == 1 and out == "",
              err.strip())


def check_set_ops_and_limit():
    a, b = [r[1] for r in rows_of("SA")], [r[1] for r in rows_of("SB")]
    cases = {"union_all": Counter((x,) for x in a + b),
             "union": Counter((x,) for x in set(a) | set(b)),
             "intersect": Counter((x,) for x in set(a) & set(b)),
             "except": Counter((x,) for x in set(a) - set(b))}
    q = "§M2.3 set operators (union_all bag; union, intersect, except set semantics; output takes the left input's names)"
    for op, expected in cases.items():
        for frames in (4, 16):
            plan = node(op, left=node("project", columns=["SA.b"], child=scan("SA")),
                        right=node("project", columns=["SB.b"], child=scan("SB")))
            rows, run, trace = run_plan(plan, frames)
            check("M2.3-setops", q, f"{op} on 30 + 20 pages with duplicates (B = {frames})", rows == expected,
                  f"{sum(rows.values())} rows")
            check("M2.3-setops-names", q, f"{op} output names come from the left input",
                  run["schema"][0][0] == "SA.b")
    code, out, err = exec_cmd("run", node("union", left=scan("R10"), right=scan("KR")), 4)
    check("M2.3-setops-types", "§M2.3: \"Both inputs must have the same number of columns with the same types, position by position\"",
          "union of 3 columns with 2 is rejected", code == 1, err.strip())

    ql = "§M2.3 limit: \"Returns the first n tuples, then stops pulling from its child. Must actually stop.\""
    rows, run, trace = run_plan(node("limit", n=3, child=scan("P80")), 4)
    check("M2.3-limit", ql, "limit 3 on an 80-page table: 3 rows and only the first page read",
          run["totals"]["tuples_out"] == 3 and sub(trace)["page_reads"] <= 2, f"reads {sub(trace)['page_reads']}")


def check_empty_inputs():
    q = "§M2.8: \"An empty input through every operator returns cleanly, including group_by.\""
    plans = {
        "seqscan": scan("E"), "filter": node("filter", condition="E.a > 1", child=scan("E")),
        "project": node("project", columns=["E.a"], child=scan("E")),
        "sort": node("sort", keys=[["E.a", "asc"]], child=scan("E")),
        "distinct sort": node("distinct", mode="sort", child=scan("E")),
        "distinct hash": node("distinct", mode="hash", child=scan("E")),
        "group_by sort": node("group_by", mode="sort", keys=["E.a"], aggregates=[["count", "*"]], child=scan("E")),
        "group_by hash": node("group_by", mode="hash", keys=["E.a"], aggregates=[["count", "*"]], child=scan("E")),
        "limit": node("limit", n=5, child=scan("E")),
    }
    for op in ("nested_loop_join", "sort_merge_join", "hash_join"):
        plans[op + " (empty left)"] = node(op, condition="E.a = R10.a", left=scan("E"), right=scan("R10"))
        plans[op + " (empty right)"] = node(op, condition="R10.a = E.a", left=scan("R10"), right=scan("E"))
    for op in ("union_all", "union", "intersect", "except"):
        plans[op] = node(op, left=scan("E"), right=scan("E2"))
    for name, plan in plans.items():
        try:
            rows, run, trace = run_plan(plan, 4)
            check("M2.8-7", q, f"{name} on an empty input", sum(rows.values()) == 0)
        except RuntimeError as e:
            check("M2.8-7", q, f"{name} on an empty input", False, str(e)[:200])


def check_validation():
    q = "§M2.2: \"rejected before execution, with a message naming the offending node and the specific problem\""
    cases = [
        ("malformed JSON", '{"op": "seqscan", "table": "R10"', "line"),
        ("missing field", json.dumps(node("sort", child=scan("R10"))), "Node 'sort': missing required field 'keys'."),
        ("field of the wrong type", json.dumps(node("filter", condition=5, child=scan("R10"))),
         "Node 'filter': field 'condition' must be a string."),
        ("unsupported operator", json.dumps(node("merge_join", condition="R10.a = S4.a", left=scan("R10"), right=scan("S4"))),
         "merge_join"),
    ]
    for name, text, expected in cases:
        code, out, err = exec_cmd("run", None, 4, plan_text=text)
        check("M2.2-validation", q, f"{name} is rejected naming the problem", code == 1 and expected in err and out == "",
              err.strip())
    qo = "§M2.2: \"A plan referring to an unknown table or column must be rejected at open, before any execution work begins.\""
    for name, plan in (("unknown table", scan("Nope")),
                       ("unknown column", node("filter", condition="R10.zz > 1", child=node("sort", keys=[["R10.a", "asc"]], child=scan("R10"))))):
        code, out, err = exec_cmd("trace", plan, 4)
        check("M2.2-open", qo, f"{name}: rejected, no output, no temp file", code == 1 and out == "" and not temp_files_left(),
              err.strip())
    qt = "§M2.1: \"INT against VARCHAR is an error, for every operator including equality.\""
    for op, plan in (("filter", node("filter", condition="R10.a = R10.b", child=scan("R10"))),
                     ("nested_loop_join", node("nested_loop_join", condition="R10.a = S4.b", left=scan("R10"), right=scan("S4"))),
                     ("sort_merge_join", node("sort_merge_join", condition="R10.a = S4.b", left=scan("R10"), right=scan("S4"))),
                     ("hash_join", node("hash_join", condition="R10.a = S4.b", left=scan("R10"), right=scan("S4")))):
        code, out, err = exec_cmd("run", plan, 4)
        check("M2.1-types", qt, f"{op}: INT = VARCHAR is an error", code == 1 and "type" in err.lower(), err.strip())


def check_formats_and_determinism():
    q = "§M2.5: run --json { \"schema\": ..., \"rows\": ..., \"totals\": {\"tuples_out\",\"comparisons\",\"page_reads\",\"page_writes\"} }"
    code, out, err = exec_cmd("run", node("filter", condition="KR.b > 1", child=scan("KR")), 4)
    j = json.loads(out)
    check("M2.5-json", q, "run --json keys and order", list(j) == ["schema", "rows", "totals"] and
          list(j["totals"]) == ["tuples_out", "comparisons", "page_reads", "page_writes"], list(j))
    check("M2.5-json-2", q, "run --json schema entries", j["schema"] == [["KR.a", "INT"], ["KR.b", "INT"]], j["schema"])
    code, out, err = exec_cmd("trace", node("hash_join", condition="KR.b = KS.b", left=scan("KR"), right=scan("KS")), 4,
                              json_flag=False)
    lines = out.splitlines()
    qt = "§M2.5: \"trace output, one line per node, in plan order, indented by depth\""
    check("M2.5-trace", qt, "trace: one line per node, plan order, indented by depth",
          len(lines) == 3 and lines[0].startswith("hash_join ") and lines[1].startswith("  seqscan KR ")
          and lines[2].startswith("  seqscan KS ") and all(k in lines[0] for k in
                                                            ("tuples_out=", "comparisons=", "reads=", "writes=")),
          " / ".join(lines))

    qd = "§M2.1: \"Same plan at same B → same counters every time.\"; §M2.8: \"Counters identical across repeated runs.\""
    for plan in (node("hash_join", condition="SKR.b = SKS.b", left=scan("SKR"), right=scan("SKS")),
                 node("sort", keys=[["P80.a", "asc"]], child=scan("P80")),
                 node("distinct", mode="hash", child=node("project", columns=["D30.b"], child=scan("D30"))),
                 node("sort_merge_join", condition="DR.b = DS.b", left=scan("DR"), right=scan("DS"))):
        outs = [exec_cmd("trace", plan, 4)[1] for _ in range(2)]
        check("M2.8-10", qd, f"{plan['op']}: two runs give identical trace output", outs[0] == outs[1])


def check_b4_shapes():
    q = "§M2.8: \"Every plan runs at B = 4.\" (§M2.1: B \"as small as 4 during grading\")"
    nlj = lambda c, l, r: node("nested_loop_join", condition=c, left=l, right=r)
    plans = {
        "3-table nested loop chain": nlj("S4.c < KR.b", nlj("R10.a < S4.a", scan("R10"), scan("S4")), scan("KR")),
        "nested loop with a sort as inner": nlj("R10.a = S4.a", scan("R10"), node("sort", keys=[["S4.a", "desc"]], child=scan("S4"))),
        "nested loop over a sort-merge join": nlj("S4.a = KR.b", node("sort_merge_join", condition="R10.a = S4.a",
                                                                      left=scan("R10"), right=scan("S4")), scan("KR")),
        "sort over a sort-merge join": node("sort", keys=[["R10.a", "desc"]], child=node(
            "sort_merge_join", condition="R10.a = S4.a", left=scan("R10"), right=scan("S4"))),
        "hash join of two hash joins": node("hash_join", condition="R10.a = R30.a",
                                            left=node("hash_join", condition="R10.a = S4.a", left=scan("R10"), right=scan("S4")),
                                            right=node("hash_join", condition="R30.a = S10.a", left=scan("R30"), right=scan("S10"))),
        "sort over union_all with a blocking right input": node("sort", keys=[["DR.a", "asc"]], child=node(
            "union_all", left=scan("DR"), right=node("group_by", mode="sort", keys=["DS.a", "DS.b"], aggregates=[],
                                                     child=node("hash_join", condition="DS.a = KS.a", left=scan("DS"),
                                                                right=scan("KS"))))),
        "group_by over a sort-merge join": node("group_by", mode="hash", keys=["R10.c"], aggregates=[["count", "*"]],
                                                child=node("sort_merge_join", condition="R10.a = S4.a", left=scan("R10"),
                                                           right=scan("S4"))),
    }
    for name, plan in plans.items():
        try:
            rows, run, trace = run_plan(plan, 4)
            check("M2.8-5", q, f"{name} runs at B = 4", trace["max_frames_held"] <= 4,
                  f"max_frames_held {trace['max_frames_held']}, {sum(rows.values())} rows")
        except RuntimeError as e:
            check("M2.8-5", q, f"{name} runs at B = 4", False, str(e)[:200])
    # The chain's rows against the Python model.
    rows, run, trace = run_plan(plans["3-table nested loop chain"], 4)
    expected = bag(l + s + k for l in rows_of("R10") for s in rows_of("S4") if l[0] < s[0]
                   for k in rows_of("KR") if s[2] < k[1])
    check("M2.8-5-rows", q, "3-table nested loop chain at B = 4: correct bag", rows == expected)


def check_memory_and_temp_files():
    q1 = "§M2.1: \"Holding more than B pages at any moment is a correctness failure\""
    over = [(p, b, m) for p, b, m, _ in MEMORY if m > b]
    check("M2.1-B", q1, f"every one of {len(MEMORY)} runs held at most B frames", not over, over[:3])
    q2 = "§M2.1: \"After close every page the operator pinned is unpinned and every temporary file it created is deleted.\""
    left = [(p, t) for p, _, _, t in MEMORY if t]
    check("M2.8-8", q2, f"no temporary file left after any of {len(MEMORY)} runs (and no exit 3: pinned pages)",
          not left, left[:3])


# ---------------------------------------------------------------- main


def build_tables():
    load_table("R10", R_COLUMNS, r_rows(2400))
    load_table("S4", R_COLUMNS, r_rows(960))
    load_table("R30", R_COLUMNS, r_rows(7200))
    load_table("S10", R_COLUMNS, r_rows(2400))
    load_table("R60", R_COLUMNS, r_rows(14400))
    load_table("S20", R_COLUMNS, r_rows(4800))
    for n in (8, 12, 30, 80, 200):
        load_table(f"P{n}", R_COLUMNS, [((i * 7919) % (n * ROWS_PER_PAGE), b, c)
                                         for i, (_, b, c) in enumerate(r_rows(n * ROWS_PER_PAGE))])
    load_table("E", R_COLUMNS, [])
    load_table("E2", R_COLUMNS, [])
    load_table("KR", K_COLUMNS, k_rows([1, 2, 2, 3]))
    load_table("KS", K_COLUMNS, k_rows([2, 2, 4]))
    load_table("DR", K_COLUMNS, k_rows([(i * 7) % 13 for i in range(400)]))
    load_table("DS", K_COLUMNS, k_rows([(i * 7) % 13 for i in range(300)]))
    load_table("LR", K_COLUMNS, k_rows([7] * 1200 + [100 + i for i in range(600)]))
    load_table("LS", K_COLUMNS, k_rows([7] * 1200 + [100 + 2 * i for i in range(600)]))
    load_table("SKR", K_COLUMNS, k_rows([7 if i < 2000 else 1000 + i - 2000 for i in range(5000)]))
    load_table("SKS", K_COLUMNS, k_rows([7 if i < 1000 else 1000 + 2 * (i - 1000) for i in range(2500)]))
    load_table("D30", K_COLUMNS, k_rows([(i * 31) % 3000 for i in range(30 * 511)]))
    load_table("SA", K_COLUMNS, k_rows([(i * 13) % 4000 for i in range(30 * 511)]))
    load_table("SB", K_COLUMNS, k_rows([(i * 17) % 5000 + 2000 for i in range(20 * 511)]))
    load_table("U", [("s", "VARCHAR(10)")], [(s,) for s in ["a", "B", "z", "é", "Z", "ab", "aB", "Ä", "0"]])
    depts = ["cs", "ee", "math", "bio"]
    load_table("G", [("dept", "VARCHAR(5)"), ("salary", "INT"), ("name", "VARCHAR(10)")],
               [(depts[i % 4], 1000 + (i * 37) % 900, f"n{(i * 53) % 97}") for i in range(3000)])


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # the Windows console default cannot print "−" or "é"
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    build_tables()
    for section in (check_scan_filter_project, check_sort, check_distinct_group_by, check_joins,
                    check_set_ops_and_limit, check_empty_inputs, check_validation, check_formats_and_determinism,
                    check_b4_shapes, check_memory_and_temp_files):
        try:
            section()
        except Exception as e:  # a crash in one section is a FAIL, the rest still run
            record(section.__name__, "", "section crashed", "FAIL", repr(e)[:300])
    print()

    counts = Counter(v for _, _, _, v, _ in RESULTS)
    lines = ["# exec acceptance against SPEC.md", "",
             f"{counts['PASS']} PASS, {counts['ALLOWED']} ALLOWED, {counts['FAIL']} FAIL "
             f"({len(RESULTS)} checks, {len(MEMORY)} plan runs).", "",
             "| Id | Check | Verdict | Evidence | Spec |", "|---|---|---|---|---|"]
    for req, quote, what, verdict, evidence in RESULTS:
        clean = lambda s: s.replace("|", "\\|").replace("\n", " ")
        lines.append(f"| {req} | {clean(what)} | **{verdict}** | {clean(evidence)[:300]} | {clean(quote)} |")
    (OUT / "acceptance.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    for req, quote, what, verdict, evidence in RESULTS:
        if verdict != "PASS":
            print(f"{verdict}: {req}: {what}\n    {evidence[:400]}")
    print(f"\n{counts['PASS']} PASS, {counts['ALLOWED']} ALLOWED, {counts['FAIL']} FAIL "
          f"-> {OUT / 'acceptance.md'}")
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
