# Lecture map: CMU 15-445/645 Intro to Database Systems (Fall 2024)

Only the lectures that explain something we build, in the order we implement the modules: common (Phase 0) → storage (M1) → exec (M2) → sql (M3) → opt (M4) → shell. Watch the lecture, build or review its chunks, then go over the code with `/teach-code`. Source: the official course schedule, https://15445.courses.cs.cmu.edu/fall2024/schedule.html (slides and notes are linked there too).

## Phase 0: common (done)

| Order | Lecture | Explains |
|---|---|---|
| 1 | [#01 Relational Model & Algebra](https://www.youtube.com/watch?v=APqWIjtzNGE&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=2) | the relational-algebra engine (select, project, join, rename, union, intersect, except), schemas and tuples |

## M1: storage (done; watch to review)

| Order | Lecture | Explains (chunks) |
|---|---|---|
| 2 | [#03 Database Storage I](https://www.youtube.com/watch?v=dSxV5Sob5V8&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=4) | files and pages, heap files and page directory, slotted pages, system catalog: 01 disk manager, 03 slotted page, 07 directory page, 08 heap file, 09 heap scan, 11–12 catalog |
| 3 | [#04 Database Storage II](https://www.youtube.com/watch?v=IHtVWGhG0Xg&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=5) | tuple layout and record format (vs log-structured storage): 02 record codec, 03 slotted page |
| 4 | [#06 Memory Management](https://www.youtube.com/watch?v=aoewwZwVmv4&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=7) | buffer pool, pinning, dirty pages, LRU and CLOCK, sequential flooding: 04 replacement policies, 05 buffer pool, 06 page guard, 10 temp files, 15 trace, 16 report (trace 3) |

## M2: exec (next)

| Order | Lecture                                                                                                                    | Explains (chunks)                                                                                                                                                |
| ----- | -------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 5     | [#13 Query Execution I](https://www.youtube.com/watch?v=HNtlew7mCc4&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=14)      | the iterator (Volcano) model, pipelining, plan trees: 01 operator interface, 02 physical plan JSON, 03 frame budget, 04 pipelined operators, 05 plan builder     |
| 6     | [#11 Sorting & Aggregations](https://www.youtube.com/watch?v=mM3sFwSuGNY&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=12) | external merge sort, runs and passes, sort- and hash-based aggregation: 06 run files, 07 external sort, 08 sort distinct and set ops, 10 group by and aggregates |
| 7     | [#07 Hash Tables](https://www.youtube.com/watch?v=VkP_yLM5KBU&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=8)             | hash functions and in-memory hash tables: 09 hash partitioning, 10 group by, 13 grace hash join                                                                  |
| 8     | [#12 Join Algorithms](https://www.youtube.com/watch?v=MFazkaZKs1s&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=13)        | block nested loop, sort-merge and (grace) hash join and their I/O costs: 11 block nested loop, 12 sort-merge join, 13 grace hash join                            |

## M3: sql

| Order | Lecture | Explains (chunks) |
|---|---|---|
| 9 | [#02 Modern SQL](https://www.youtube.com/watch?v=MzigBKf84aY&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=3) | the SQL we accept and what it means: 04 statements, 08 type and aggregate checks, 10 subqueries and set ops, 11 DDL and DML. Rewatch #01 before 06 common logical ops and 09 translate select block (SQL → relational algebra). |

The lexer and parser (chunks 01–05) are not covered by the course; read *Crafting Interpreters*, chapters "Scanning" and "Parsing Expressions" (hand-written recursive descent, as our rules require).

## M4: opt

| Order | Lecture | Explains (chunks) |
|---|---|---|
| 10 | [#15 Query Planning & Optimization](https://www.youtube.com/watch?v=X8EHO8VLeG0&list=PLSE8ODhjZXjYDBpQnSymaectKjxCy6BYq&index=16) | statistics, histograms, selectivity, rewrite rules, cost model, dynamic-programming join ordering: all of opt 01–13. Rewatch #11 (histograms are built with the external sort, 02–03) and #12 (join cost formulas, 09). |

## Shell

No new lecture: it connects sql → opt → exec behind one prompt.
