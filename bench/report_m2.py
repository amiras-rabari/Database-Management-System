"""Produces the five measurements of REPORT_M2.md (SPEC §M2.6).

Usage (from the repo root, after building):
    python bench/report_m2.py [path/to/exec.exe] [path/to/storage.exe]

Like bench/report_m1.py, it only calls the command-line tools: `storage` to
build the tables, `exec trace --json` to run each plan and read its counters.
It reuses the table loader and runner of bench/exec_acceptance.py. Every
measurement is run twice and must give identical counters (§M2.1). Tables are
printed as Markdown and also written to bench/out/m2/tables.md; the two
crossover plots go to bench/plots/ (SVG, no plotting library needed).

Data (the M2 dataset is NOT AVAILABLE): R and S have the schema
(a INT, b VARCHAR(10), c INT) with 3-byte b values, so a record is 13 bytes and
240 rows fill a page (§M1.4: 4088 // (13 + 4)). Row i has a = i, so the join
R.a = S.a has exactly |S| matches. The skew tables are (a INT, b INT), 340 rows
per page (4088 // (8 + 4)). No random numbers: every table is a formula.
"""

import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # find exec_acceptance.py beside this file
import exec_acceptance as ea  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ea.OUT = ROOT / "bench" / "out" / "m2"
ea.DB = ea.OUT / "db"
if len(sys.argv) > 1:
    ea.EXEC = sys.argv[1]
if len(sys.argv) > 2:
    ea.STORAGE = sys.argv[2]
PLOTS = ROOT / "bench" / "plots"

JOINS = ["nested_loop_join", "sort_merge_join", "hash_join"]
SHORT = {"nested_loop_join": "BNL", "sort_merge_join": "SMJ", "hash_join": "Hash"}


def trace(plan, frames):
    """`exec trace --json` twice; the two outputs must be identical."""
    outputs = []
    for _ in range(2):
        code, out, err = ea.exec_cmd("trace", plan, frames)
        if code != 0:
            sys.exit(f"exec trace failed ({code}): {err}\nplan: {json.dumps(plan)}")
        outputs.append(out)
    assert outputs[0] == outputs[1], f"counters differ between two runs: {json.dumps(plan)}"
    assert not ea.temp_files_left(), "a temporary file was left behind"
    result = json.loads(outputs[0])
    assert result["max_frames_held"] <= frames, "held more than B frames"
    return result


def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    lines += ["| " + " | ".join(str(v) for v in row) + " |" for row in rows]
    return "\n".join(lines)


def passes(pages, frames):
    """§M2.3: 1 + ceil(log(ceil(N/B)) / log(B-1)), with whole numbers."""
    runs = math.ceil(pages / frames)
    count = 1
    while runs > 1:
        runs = math.ceil(runs / (frames - 1))
        count += 1
    return count


def sort_cost(pages, frames):
    """§M4.4 (must match M2): 2 * M * (passes - 1) + M."""
    return 2 * pages * (passes(pages, frames) - 1) + pages


def formula(op, m, n, frames):
    if op == "nested_loop_join":
        return m + math.ceil(m / (frames - 2)) * n
    if op == "sort_merge_join":
        return sort_cost(m, frames) + sort_cost(n, frames) + m + n
    return 3 * (m + n)


def join_io(op, left, right, frames):
    """Measured page reads and writes of the join's line (D1 (b): the join
    plus its inputs, as in §M2.5's trace) and the join's details."""
    t = trace(ea.node(op, condition=f"{left}.a = {right}.a", left=ea.scan(left), right=ea.scan(right)), frames)
    sub = t["nodes"][0]["subtree"]
    return sub["page_reads"], sub["page_writes"], t["nodes"][0]["details"]


def rs_table(name, pages):
    ea.load_table(name, ea.R_COLUMNS, ea.r_rows(pages * ea.ROWS_PER_PAGE))


# ---------------------------------------------------------------- 1. cost

SIZES = [("R20", 20, "S10", 10), ("R60", 60, "S30", 30), ("R180", 180, "S90", 90)]
BS = [4, 16, 64, 256]


def cost_table():
    rows = []
    for left, m, right, n in SIZES:
        for frames in BS:
            for op in JOINS:
                reads, writes, details = join_io(op, left, right, frames)
                expected = formula(op, m, n, frames)
                measured = reads if op == "nested_loop_join" else reads + writes
                note = ""
                if op == "hash_join":
                    note = (f"partitions {sum(details['left_pages'])} + {sum(details['right_pages'])} pages; "
                            f"overflow {len(details['overflow_partitions'])} of {details['partitions']}")
                rows.append([SHORT[op], frames, m, n, reads, writes, expected, measured - expected, note])
    return table(["Join", "B", "M", "N", "Reads", "Writes", "Formula", "Measured − formula", "Notes"], rows)


# ---------------------------------------------------------------- 2. passes

SORTS = [(8, 4), (12, 4), (80, 4), (30, 8), (60, 16), (200, 16), (200, 64), (800, 32)]


def passes_table():
    rows = []
    for pages, frames in SORTS:
        name = f"P{pages}"
        if name not in ea.TABLES:
            rs_table(name, pages)
        t = trace(ea.node("sort", keys=[[f"{name}.a", "desc"]], child=ea.scan(name)), frames)
        d = t["nodes"][0]["details"]
        rows.append([pages, frames, "yes" if pages == 20 * frames else "", d["passes"], passes(pages, frames),
                     d["initial_runs"], math.ceil(pages / frames), math.ceil(pages / (frames - 1)),
                     t["nodes"][0]["page_reads"], t["nodes"][0]["page_writes"]])
    return table(["N", "B", "N = 20B", "Measured passes", "Formula passes", "Measured runs after pass 0",
                  "ceil(N/B)", "ceil(N/(B−1))", "Sort's own reads", "Sort's own writes"], rows)


# ---------------------------------------------------------------- 3. crossover

CROSS_B = [4, 5, 6, 8, 12, 16, 24, 32, 48, 64, 128, 256]
CROSS_SIZES = [5, 10, 20, 40, 80, 160]
CROSS_SIZE_B = 16


def crossover():
    """Total measured I/O (reads + writes) of each join."""
    by_b = {op: [] for op in JOINS}
    for frames in CROSS_B:
        for op in JOINS:
            reads, writes, _ = join_io(op, "R60", "S30", frames)
            by_b[op].append(reads + writes)
    by_size = {op: [] for op in JOINS}
    for m in CROSS_SIZES:
        left, right = f"CR{m}", f"CS{m}"
        if left not in ea.TABLES:
            rs_table(left, m)
            rs_table(right, m // 2 if m >= 2 else 1)
        for op in JOINS:
            reads, writes, _ = join_io(op, left, right, CROSS_SIZE_B)
            by_size[op].append(reads + writes)

    def rows_of(xs, data):
        rows = []
        for i, x in enumerate(xs):
            costs = {op: data[op][i] for op in JOINS}
            best = min(costs.values())
            cheapest = ", ".join(SHORT[op] for op in JOINS if costs[op] == best)
            rows.append([x, costs["nested_loop_join"], costs["sort_merge_join"], costs["hash_join"], cheapest])
        return rows

    svg_plot(PLOTS / "m2_crossover_vary_b.svg", "Join I/O, R = 60 pages, S = 30 pages, varying B",
             "B (frames)", CROSS_B, by_b)
    svg_plot(PLOTS / "m2_crossover_vary_size.svg", f"Join I/O at B = {CROSS_SIZE_B}, S = R / 2, varying R",
             "R (pages)", CROSS_SIZES, by_size)
    header = ["", "BNL I/O", "SMJ I/O", "Hash I/O", "Cheapest"]
    return (table(["B"] + header[1:], rows_of(CROSS_B, by_b)),
            table(["M (N = M/2)"] + header[1:], rows_of(CROSS_SIZES, by_size)))


def svg_plot(path, title, xlabel, xs, series):
    """A small log-log line chart, one line per join."""
    width, height, left, right, top, bottom = 640, 400, 70, 150, 40, 50
    ys = [v for vs in series.values() for v in vs]
    lx0, lx1 = math.log10(min(xs)), math.log10(max(xs))
    ly0, ly1 = math.log10(min(ys)), math.log10(max(ys))
    ly1 += (ly1 - ly0) * 0.05 or 1

    def px(x):
        return left + (math.log10(x) - lx0) / (lx1 - lx0) * (width - left - right)

    def py(y):
        return height - bottom - (math.log10(y) - ly0) / (ly1 - ly0) * (height - top - bottom)

    colors = {"nested_loop_join": "#d62728", "sort_merge_join": "#1f77b4", "hash_join": "#2ca02c"}
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" font-family="sans-serif" font-size="12">',
           '<rect width="100%" height="100%" fill="white"/>',
           f'<text x="{width / 2 - 60}" y="22" font-size="14" text-anchor="middle">{title}</text>',
           f'<line x1="{left}" y1="{height - bottom}" x2="{width - right}" y2="{height - bottom}" stroke="black"/>',
           f'<line x1="{left}" y1="{top}" x2="{left}" y2="{height - bottom}" stroke="black"/>',
           f'<text x="{(left + width - right) / 2}" y="{height - 12}" text-anchor="middle">{xlabel}</text>',
           f'<text x="16" y="{(top + height - bottom) / 2}" transform="rotate(-90 16 {(top + height - bottom) / 2})" '
           f'text-anchor="middle">page I/O (reads + writes, log scale)</text>']
    for x in xs:
        out.append(f'<text x="{px(x):.1f}" y="{height - bottom + 16}" text-anchor="middle">{x}</text>')
    for e in range(math.floor(ly0), math.ceil(ly1) + 1):
        for f in (1, 2, 5):
            y = f * 10 ** e
            if ly0 <= math.log10(y) <= ly1:
                out.append(f'<line x1="{left - 4}" y1="{py(y):.1f}" x2="{width - right}" y2="{py(y):.1f}" stroke="#ddd"/>')
                out.append(f'<text x="{left - 8}" y="{py(y) + 4:.1f}" text-anchor="end">{y}</text>')
    for i, (op, vs) in enumerate(series.items()):
        points = " ".join(f"{px(x):.1f},{py(v):.1f}" for x, v in zip(xs, vs))
        out.append(f'<polyline points="{points}" fill="none" stroke="{colors[op]}" stroke-width="2"/>')
        out += [f'<circle cx="{px(x):.1f}" cy="{py(v):.1f}" r="3" fill="{colors[op]}"/>' for x, v in zip(xs, vs)]
        out.append(f'<rect x="{width - right + 15}" y="{top + 20 * i}" width="12" height="12" fill="{colors[op]}"/>')
        out.append(f'<text x="{width - right + 32}" y="{top + 20 * i + 11}">{SHORT[op]}</text>')
    out.append("</svg>")
    PLOTS.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- 4. skew

def skew_table():
    """40 % of the rows on both sides share b = 7 (§M2.6 measurement 4);
    the uniform tables have the same sizes and all-distinct keys."""
    per_page = 4088 // (8 + 4)
    r_rows, s_rows = 20 * per_page, 10 * per_page
    ea.load_table("SKR", ea.K_COLUMNS, ea.k_rows([7 if i < 0.4 * r_rows else 100000 + i for i in range(r_rows)]))
    ea.load_table("SKS", ea.K_COLUMNS, ea.k_rows([7 if i < 0.4 * s_rows else 100000 + 2 * i for i in range(s_rows)]))
    ea.load_table("UNR", ea.K_COLUMNS, ea.k_rows(list(range(r_rows))))
    ea.load_table("UNS", ea.K_COLUMNS, ea.k_rows([2 * i for i in range(s_rows)]))
    rows = []
    for frames in (4, 8, 16, 64):
        for label, left, right in (("uniform", "UNR", "UNS"), ("40 % skew", "SKR", "SKS")):
            t = trace(ea.node("hash_join", condition=f"{left}.b = {right}.b", left=ea.scan(left),
                              right=ea.scan(right)), frames)
            d, sub = t["nodes"][0]["details"], t["nodes"][0]["subtree"]
            rows.append([frames, label, d["partitions"], " ".join(map(str, d["left_pages"])),
                         " ".join(map(str, d["right_pages"])),
                         ", ".join(map(str, d["overflow_partitions"])) or "none",
                         sub["page_reads"], sub["page_writes"], sub["page_reads"] + sub["page_writes"],
                         t["nodes"][0]["comparisons"], t["nodes"][0]["tuples_out"]])
    return table(["B", "Data", "P", "Left partition pages", "Right partition pages", "Overflowed (block nested loop)",
                  "Reads", "Writes", "I/O", "Comparisons", "Rows"], rows), r_rows, s_rows


# ---------------------------------------------------------------- 5. budget

def budget_table():
    rows = []
    for name, plan in (
            ("sort of 80 pages (20 × B)", ea.node("sort", keys=[["P80.a", "desc"]], child=ea.scan("P80"))),
            ("hash join over that sort", ea.node("hash_join", condition="P80.a = R20.a",
                                                 left=ea.node("sort", keys=[["P80.a", "desc"]], child=ea.scan("P80")),
                                                 right=ea.scan("R20")))):
        t = trace(plan, 4)
        rows.append([name, 4, t["max_frames_held"], t["totals"]["page_reads"], t["totals"]["page_writes"],
                     t["nodes"][0]["tuples_out"]])
    return table(["Plan", "B", "Largest number of frames held at once", "Reads", "Writes", "Rows"], rows)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    import shutil
    shutil.rmtree(ea.OUT, ignore_errors=True)
    ea.OUT.mkdir(parents=True)
    for left, m, right, n in SIZES:
        rs_table(left, m)
        rs_table(right, n)
    sections = [("1. Cost against formula", cost_table())]
    sections.append(("2. Sort passes", passes_table()))
    vary_b, vary_size = crossover()
    sections.append(("3a. Crossover, R = 60 and S = 30 pages, varying B", vary_b))
    sections.append((f"3b. Crossover at B = {CROSS_SIZE_B}, varying size", vary_size))
    skew, r_rows, s_rows = skew_table()
    sections.append((f"4. Skew (hash join, R = {r_rows} rows = 20 pages, S = {s_rows} rows = 10 pages)", skew))
    sections.append(("5. The budget", budget_table()))
    text = "\n\n".join(f"## {title}\n\n{body}" for title, body in sections) + "\n"
    (ea.OUT / "tables.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
