# GRAMMAR_SQL

# Grammar and Language Design for the SQL Front End

## 1. Scope

This document defines the concrete syntax and the parsing rules for the SQL subset of Module 3 (`modules/sql`, SPEC §M3.1–M3.2). The hand-written lexer (`modules/sql/lexer/sql_lexer.cpp`) and recursive-descent parser (`modules/sql/parser/sql_parser.cpp`) implement these rules directly. Precedence and associativity come from the grammar itself, through the way its rules nest. They are not special cases or a precedence table hidden inside the parser.

There are two entry points:

- `script` for a whole SQL text: any number of statements separated by `;`. `sql parse --query`, `sql compile` and `sql check` (on a file) and the shell use it.
- `single_query` for exactly one query (`parseSqlQuery`, used by the sql tests).

The parser only answers "is this text shaped like SQL?". It needs no catalog, so `sql parse` works without a database (§M3.7). Name resolution, type checks and aggregate checks run after parsing, against the catalog (section 7).

This subset intentionally has no NULLs, no arithmetic expressions, no subqueries inside conditions, no `IN`, `LIKE` or `BETWEEN`, no outer joins and no `UPDATE` or `DELETE`.

## 2. Lexical Conventions

The lexer reads the text once from left to right. At each position the current character picks exactly one rule, so it never has to back up. Every token records the line and column of its first character, and every error is reported at a position (§M3.6). Lines and columns start at 1, a column counts bytes, a tab counts as one column, and `\r\n` counts as one newline.

### 2.1 Whitespace and comments

Spaces, tabs, `\r` and `\n` separate tokens and are otherwise ignored. A newline has no meaning in this language, so a statement may span many lines.

`--` starts a comment that runs to the end of the line and produces no tokens. The comment rule is checked before the integer rule, so `--5` is a comment, not `-` followed by `-5`.

### 2.2 Identifiers

An identifier is a letter or an underscore, followed by zero or more letters, digits or underscores (§M3.1 row 2).

```ebnf
identifier ::= ( letter | "_" ) { letter | digit | "_" }

letter     ::= "A".."Z" | "a".."z"
digit      ::= "0".."9"
```

Examples: `Emp`, `e`, `avg_sal`, `_tmp`, `table2`. An identifier cannot begin with a digit, and `.` is not part of an identifier: `e.name` is three tokens, `e`, `.`, `name`.

**Identifiers are case sensitive.** `Emp` and `emp` are two different tables, and `Name` and `name` are two different columns. A word that spells a keyword (in any case) is never an identifier (section 2.3).

### 2.3 Keywords

The keywords are every upper-case word of the §M3.2 grammar:

`SELECT`, `DISTINCT`, `FROM`, `WHERE`, `GROUP`, `BY`, `HAVING`, `ORDER`, `ASC`, `DESC`, `LIMIT`, `UNION`, `ALL`, `INTERSECT`, `EXCEPT`, `JOIN`, `ON`, `AS`, `AND`, `OR`, `NOT`, `CREATE`, `TABLE`, `INSERT`, `INTO`, `VALUES`, `INT`, `VARCHAR`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`

**Keywords are case insensitive** (§M3.1 row 1). The lexer reads a whole word, upper-cases only `a`..`z` (not through the locale, so the result is the same on every machine), and looks it up in the keyword table. `SELECT`, `select` and `SeLeCt` are therefore the same token.

**All keywords are reserved.** A table, column or alias can never be called `count`, `int` or `order`, in any case. Section 10 explains why (difference D5). For example:

```
SELECT count FROM R
→ error: syntax: expected '(' but found 'FROM' at line 1, column 14
```

because `count` is the keyword `COUNT`, so the parser expects an aggregate call.

### 2.4 Integer literals

An integer is an optional sign written directly before one or more digits:

```ebnf
integer ::= [ "+" | "-" ] digit { digit }
```

Examples: `0`, `30`, `007` (the value 7), `+30` (the value 30), `-5`.

- The sign belongs to the number only when a digit follows it directly. `a > -5` is `a`, `>`, `-5`. **There is no arithmetic**, so a lone `-` or `+`, or `- 5` with a space, is a lexical error:

  ```
  SELECT a FROM R WHERE a > - 5
  → error: lexical: unexpected character '-' at line 1, column 27
  ```

- The value must fit in a 32-bit `INT` (−2147483648 … 2147483647), the only integer type of the project. A larger number is a lexical error at the number: `integer 3000000000 is outside the INT range`.
- Digits that run straight into a letter are a mistake, not a number followed by a word: `12abc` gives `number '12' runs straight into a letter`.

**A number inside quotes is a string.** `'30'` is a `VARCHAR` value. It is not an integer, so `age > '30'` compares an `INT` with a `VARCHAR`, which is a type error (required case 10) and not a silent conversion.

### 2.5 Quoted strings

A string begins and ends with a single quote. Two single quotes in a row inside a string stand for one literal quote (§M3.1 row 3).

```ebnf
string ::= "'" { string_char | "''" } "'"

string_char = any character except a single quote
''          = one literal single quote
'           = closes the string
```

Examples: `'Bob'`, `'a,b'`, `'x = 1 OR y'`, `'O''Brien'` (the value `O'Brien`), `''` (the empty string).

The lexer reads the characters from left to right. When it sees `'`, it checks the next character: if that is also `'`, both are taken as one literal quote; otherwise the `'` closes the string. Everything between the quotes is string content, including `)`, `,`, `--`, keywords and operators. A string may also contain a newline.

An unterminated string is a lexical error, reported at the **opening** quote, because that is where the user must look to fix it:

```
SELECT a FROM R WHERE b = 'abc
→ error: lexical: unterminated string at line 1, column 27
```

### 2.6 Comparison operators and maximal munch

The only comparison operators are those of §M3.2:

`=`, `<>`, `<`, `<=`, `>`, `>=`

The lexer uses **maximal munch**: it always takes the longest operator that fits. After `<` it looks at the next character: `=` makes `<=`, `>` makes `<>`, and anything else leaves a plain `<`. After `>` it checks for `=` in the same way. So `<=`, `>=` and `<>` are always single tokens, never two.

**`!=` is not an operator in this language.** `!` is not a character of any token, so `a != 1` is a lexical error at the `!` (section 10, D7):

```
SELECT a FROM R WHERE a != 1
→ error: lexical: unexpected character '!' at line 1, column 25
```

Any other character that starts no token (`#`, `"`, `@`, …) is a lexical error as well. The lexer never invents an operator.

### 2.7 Punctuation

The following symbols are single-character tokens:

`(`, `)`, `,`, `.`, `;`, `*`

- `(` and `)` group conditions and queries, and delimit the column list of `CREATE TABLE`, the values of `INSERT`, a `VARCHAR` length and an aggregate's argument.
- `,` separates list items: select items, FROM entries, GROUP BY and ORDER BY columns, column definitions, values.
- `.` separates a qualifier from a column name (`e.name`).
- `;` ends a statement (section 2.8).
- `*` is `SELECT *` or `COUNT(*)`.

### 2.8 Statements and semicolons

Statements are separated by `;`, and one text may hold several (§M3.1 row 7). The final `;` is optional, and empty statements (`;;`, or a text that is only comments) are skipped. A text with no statement at all is valid and holds zero statements.

## 3. Complete EBNF

Notation: `{ x }` means zero or more `x`, `[ x ]` means an optional `x`, `|` separates alternatives, upper-case words are keywords, quoted symbols are punctuation or operators, and `EOF` is the end of the text. `identifier`, `integer` and `string` are the tokens of section 2.

### 3.1 Entry points

```ebnf
script       ::= [ statement ] { ";" [ statement ] } EOF

single_query ::= query [ ";" ] EOF

statement    ::= create
              |  insert
              |  query
```

The first token of a statement decides which alternative applies: `CREATE`, `INSERT`, or `SELECT` / `(` for a query. Since `;` is required only *between* two statements, `SELECT a FROM R`, `SELECT a FROM R;` and `;;SELECT a FROM R;;` are all one statement, while `SELECT a FROM R SELECT b FROM S` is a syntax error at the second `SELECT` (`expected ';' or end of input`).

### 3.2 CREATE TABLE and INSERT

```ebnf
create      ::= CREATE TABLE identifier "(" column_def { "," column_def } ")"

column_def  ::= identifier column_type

column_type ::= INT
             |  VARCHAR "(" integer ")"

insert      ::= INSERT INTO identifier VALUES "(" literal { "," literal } ")"

literal     ::= integer
             |  string
```

The grammar requires at least one column and at least one value. `INSERT` takes exactly one row: a second `( … )` is left over and fails at the statement end.

The `VARCHAR` length is kept as written. Whether it lies in 1..255 is a schema rule, checked later by common's `Schema` (an `error: schema:`), not by the grammar. The same holds for "the table exists" and "the number and types of the values match the table".

### 3.3 Column references and aggregates

```ebnf
column         ::= identifier [ "." identifier ]

aggregate      ::= aggregate_name "(" ( column | "*" ) ")"

aggregate_name ::= COUNT | SUM | AVG | MIN | MAX
```

Examples: `age`, `e.age`, `COUNT(*)`, `AVG(salary)`, `max(e.name)`.

The grammar allows `*` for all five functions. Only `COUNT(*)` has a meaning, so `SUM(*)` is parsed and then rejected by the type checker (section 7). Whether a column or qualifier exists is decided by name resolution, not here.

### 3.4 Queries and set operators

```ebnf
query         ::= query_operand { set_operator query_operand }

set_operator  ::= UNION [ ALL ]
               |  INTERSECT
               |  EXCEPT

query_operand ::= select_block
               |  "(" query ")"
```

`UNION ALL` is one set operator written as two keywords. After `UNION`, the parser checks whether `ALL` follows. UNION and UNION ALL become different tree nodes and later different plan nodes (`union` / `union_all`, §M3.4).

All four set operators sit in the **same** repetition, so they have one precedence level and fold to the left (section 4.1). Parentheses start a new `query` and override that: `A UNION (B EXCEPT C)`.

### 3.5 Select blocks

```ebnf
select_block ::= SELECT [ DISTINCT ] select_list
                 FROM from_list
                 [ WHERE condition ]
                 [ GROUP BY column_list [ HAVING condition ] ]
                 [ ORDER BY sort_list ]
                 [ LIMIT limit_count ]

select_list  ::= "*"
              |  select_item { "," select_item }

select_item  ::= ( column | aggregate ) [ AS identifier ]

column_list  ::= column { "," column }

sort_list    ::= sort_item { "," sort_item }

sort_item    ::= column [ ASC | DESC ]

limit_count  ::= integer                 (* value 0 or more *)
```

The clauses have one fixed order. A clause written out of order is not part of the select block, so it is left over and fails at the statement end:

```
SELECT a FROM R ORDER BY a LIMIT 5 WHERE a = 1
→ error: syntax: expected ';' or end of input but found 'WHERE' at line 1, column 36
```

`HAVING` exists only inside the GROUP BY clause. `HAVING` without `GROUP BY` therefore fails in the same way, at the `HAVING`.

A select item's alias needs `AS`. In `SELECT name n FROM R` the `n` is not an alias, so the parser reports `expected FROM but found 'n'`.

`ORDER BY` and `LIMIT` belong to the select block they are written in. In `SELECT a FROM R UNION SELECT a FROM S ORDER BY a`, the ORDER BY sorts only the second block, which is what the §M3.2 grammar says. Standard SQL and PostgreSQL would apply it to the whole union. To sort a union, write it as a FROM subquery: `SELECT u.a FROM (… UNION …) AS u ORDER BY u.a`.

### 3.6 The FROM clause

```ebnf
from_list  ::= table_ref [ comma_tail | join_tail ]

comma_tail ::= "," table_ref { "," table_ref }

join_tail  ::= JOIN table_ref ON condition { JOIN table_ref ON condition }

table_ref  ::= identifier [ [ AS ] identifier ]
            |  "(" query ")" AS identifier
```

The token after the first `table_ref` decides the form: `,` means a comma list, `JOIN` means a JOIN chain, and anything else means a single table. **The two forms never mix.** `FROM R, S JOIN T ON …` fails at the `JOIN` (`expected ';' or end of input`). Keeping them apart is what lets the translator emit `cross` for commas and `join` for `JOIN … ON` (examples 5.3 and 5.2).

Every `JOIN` needs its own `ON condition`. That condition belongs to the table it joins in: the ON after the k-th JOIN may use only the tables before it and the one it joins (sql D5).

A base table's alias may be written with or without `AS` (`Emp e`, `Emp AS e`). A FROM subquery **must** have `AS` and an alias (§M3.3):

```
SELECT a FROM (SELECT a FROM R)
→ error: syntax: expected AS and an alias (a subquery in FROM needs one) but found end of input at line 1, column 32
```

### 3.7 Conditions

```ebnf
condition           ::= or_condition

or_condition        ::= and_condition { OR and_condition }

and_condition       ::= not_condition { AND not_condition }

not_condition       ::= NOT not_condition
                     |  condition_primary

condition_primary   ::= "(" condition ")"
                     |  comparison

comparison          ::= operand comparison_operator operand

comparison_operator ::= "=" | "<>" | "<" | "<=" | ">" | ">="

operand             ::= column
                     |  literal
                     |  aggregate
```

The same `condition` rule is used for `WHERE`, `ON` and `HAVING`.

A comparison has exactly one operator between two operands. A chained comparison such as `a < b < c` is therefore not generated by the grammar: the parser finishes `a < b` and then finds a `<` it cannot use (`expected ';' or end of input but found '<'`).

An operand never starts with `(`, because there are no subqueries or arithmetic in conditions. So in `condition_primary` a `(` always opens a nested condition, and one token of lookahead is enough.

## Operator Precedence and Associativity Overview

The language has two separate precedence hierarchies: **condition precedence** (inside WHERE, ON and HAVING) and **query precedence** (set operators between whole queries).

### 1. Condition Precedence

From highest to lowest:

| Precedence | Operator(s) | Associativity |
| --- | --- | --- |
| Highest | Parentheses `( ... )` | — |
| 2 | `NOT` | Right (prefix, nests to the right) |
| 3 | `AND` | Left |
| Lowest | `OR` | Left |

Comparisons (`operand op operand`) are the atoms that `NOT`, `AND` and `OR` combine.

Thus:

```
a = 1 OR b = 2 AND NOT c = 3
```

is parsed as:

```
a = 1 OR (b = 2 AND (NOT c = 3))
```

and:

```
NOT (a = 1 OR b = 2) AND c = 3
```

is parsed as:

```
(NOT (a = 1 OR b = 2)) AND c = 3
```

### 2. Query (Set-Operator) Precedence

| Precedence | Operator(s) | Associativity |
| --- | --- | --- |
| Highest | a select block, or a parenthesised query `( query )` | — (explicitly delimited) |
| Lowest | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT` | Left, **all four at one level** |

Thus:

```
A UNION B EXCEPT C
```

is parsed as:

```
(A UNION B) EXCEPT C
```

and, because INTERSECT has no higher level here:

```
A UNION ALL B INTERSECT C
```

is parsed as:

```
(A UNION ALL B) INTERSECT C
```

(here `A`, `B` and `C` stand for select blocks). Explicit parentheses always override the levels.

### Summary

```
CONDITIONS (highest → lowest)

( ... )
   ↓
  NOT
   ↓
  AND
   ↓
  OR

QUERIES (highest → lowest)

select block / ( query )
          ↓
UNION / UNION ALL / INTERSECT / EXCEPT   (one level, left to right)
```

## 4. Precedence and Associativity

§M3.2 requires both rules to be "enforced in the grammar, not in the parser code". Each row below names the grammar rule that enforces it.

### 4.1 Set-operator precedence

| **Level** | **Construct / Operator** | **Associativity** | **Enforcing Grammar Rule** |
| --- | --- | --- | --- |
| **1** | a select block, `( query )` | Explicitly delimited / atomic | `query_operand` |
| **2** | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT` | Left | `query` (one repetition `{ set_operator query_operand }`) |

All four operators appear in **one** repetition of the `query` rule, and nowhere else. So no operator can bind tighter than another, and the repetition read from left to right makes the chain left-deep: `A UNION B EXCEPT C` means `(A UNION B) EXCEPT C`, and `A EXCEPT B EXCEPT C` means `(A EXCEPT B) EXCEPT C`.

This follows the spec: "Set operators are left associative, all at the same precedence". Standard SQL and PostgreSQL instead give `INTERSECT` a higher level than `UNION` and `EXCEPT`. Section 5.4 shows that the choice changes results. A user who wants the PostgreSQL grouping writes the parentheses: `A UNION ALL (B INTERSECT C)`.

### 4.2 Condition precedence

| **Level** | **Construct / Operator** | **Associativity** | **Enforcing Grammar Rule** |
| --- | --- | --- | --- |
| **1** | comparison (`=`, `<>`, `<`, `<=`, `>`, `>=`), `( condition )` | Not applicable (exactly one operator between two operands) | `comparison`, `condition_primary` |
| **2** | `NOT` | Right | `not_condition` (calls itself) |
| **3** | `AND` | Left | `and_condition` (repetition of `not_condition`) |
| **4** | `OR` | Left | `or_condition` (repetition of `and_condition`) |

Each level is built only out of the next tighter level. An `OR` operand is an `and_condition`, so an `AND` is always grouped inside an `OR` operand and never around it. An `AND` operand is a `not_condition`, so a `NOT` applies only to the one `not_condition` after it. Parentheses lead back to the top `condition` rule and override the levels.

Thus `NOT a = 1 AND b = 2 OR c = 3` parses as `((NOT a = 1) AND b = 2) OR c = 3`, and `NOT (a = 1 AND b = 2) OR c = 3` parses as `(NOT (a = 1 AND b = 2)) OR c = 3`.

The right recursion in `not_condition` makes repeated `NOT`s nest to the right: `NOT NOT a = 1` is `NOT (NOT a = 1)`. The repetitions in `and_condition` and `or_condition` make chains left-deep: `a AND b AND c` is `(a AND b) AND c`. For AND and OR the grouping of a chain does not change the result, but the tree shape is fixed, so the plan printed for it is always the same text (deterministic output, the same shape as common's `andOf` / `orOf`).

## 5. Why This Grammar Is Unambiguous

### 5.1 The ambiguous sketch

The handout's sketch (§M3.2) writes the set operators as:

```
query UNION [ALL] query | INTERSECT | EXCEPT
```

that is, roughly:

```
Query ::= Query "UNION" [ "ALL" ] Query
        | Query "INTERSECT" Query
        | Query "EXCEPT" Query
        | select_block
```

and describes conditions only in words: "disjunction of conjunctions of comparisons, with NOT and parentheses". For `A UNION B EXCEPT C`, the rule `Query ::= Query op Query` gives two different trees.

### 5.2 First parse tree: (A UNION B) EXCEPT C

```
          EXCEPT
         /      \
     UNION        C
    /     \
   A       B
```

### 5.3 Second parse tree: A UNION (B EXCEPT C)

```
        UNION
       /     \
      A      EXCEPT
            /      \
           B        C
```

Therefore the sketch is ambiguous.

### 5.4 Concrete data showing the ambiguity matters

Use three one-column tables, each a select block `SELECT x FROM …`:

```
A(x) = { 1 }
B(x) = { 2 }
C(x) = { 1 }
```

First grouping:

```
(A UNION B) EXCEPT C
= {1, 2} EXCEPT {1}
= {2}
```

Second grouping:

```
A UNION (B EXCEPT C)
= {1} UNION ({2} EXCEPT {1})
= {1, 2}
```

The results differ (`{2}` against `{1, 2}`), so the grouping matters.

Because SQL works on **bags**, the grouping of UNION ALL with a set operator also changes how many copies of a row survive. Take:

```
A(x) = { 1 }
B(x) = { 1 }
C(x) = { 1 }
```

```
(A UNION ALL B) INTERSECT C          A UNION ALL (B INTERSECT C)
= {1, 1} INTERSECT {1}               = {1} UNION ALL ({1} INTERSECT {1})
= {1}     (INTERSECT has set         = {1} UNION ALL {1}
           semantics, §M3.4)         = {1, 1}
```

Our grammar picks the left tree (`{1}`), as the spec's one-level rule requires. PostgreSQL, where INTERSECT binds tighter, picks the right one (`{1, 1}`).

The same kind of example exists for conditions. Take one row with `a = 1, b = 0, c = 3` and the condition `a = 1 OR b = 2 AND NOT c = 3`:

```
a = 1 OR (b = 2 AND (NOT c = 3))     = true OR (false AND false)  = true    (our grammar)
(a = 1 OR b = 2) AND (NOT c = 3)     = (true OR false) AND false  = false   (wrong grouping)
```

So the row is kept or dropped depending on the grouping. The precedence rule is what makes the result well defined.

### 5.5 How the final grammar removes the ambiguity

The final grammar never writes `Query op Query`. A set operator always has a `query_operand` on its right, and a `query_operand` is a single select block or a parenthesised query:

```ebnf
query         ::= query_operand { set_operator query_operand }

query_operand ::= select_block
               |  "(" query ")"
```

For `A UNION B EXCEPT C` there is now only one derivation. `query` reads `A`, then repeats twice: `UNION B`, then `EXCEPT C`. Each repetition takes everything built so far as its left input, so the only tree is `(A UNION B) EXCEPT C`.

For conditions, the words "disjunction of conjunctions" become the four layers of section 3.7. Each layer may contain only the next tighter one, so each condition has exactly one tree.

### 5.6 Worked parses (the tree `sql parse --explain` prints)

`explainAst` prints the tree the parser built: one line per node, two spaces of indent per level. A condition is printed on one line, and every AND, OR and NOT below the top gets parentheses, so the grouping is visible.

`sql parse --explain --query "SELECT a FROM R WHERE a = 1 OR b = 2 AND NOT c = 3"`:

```
select_block
  item: a
  from
    table: R
  where: a = 1 OR (b = 2 AND (NOT c = 3))
```

`sql parse --explain --query "SELECT x FROM A UNION SELECT x FROM B EXCEPT SELECT x FROM C"`:

```
except
  union
    select_block
      item: x
      from
        table: A
    select_block
      item: x
      from
        table: B
  select_block
    item: x
    from
      table: C
```

The `union` node is the left child of `except`: `(A UNION B) EXCEPT C`.

### 5.7 Parentheses override the grammar levels

`SELECT x FROM A UNION ALL (SELECT x FROM B INTERSECT SELECT x FROM C)` forces the other tree:

```
union_all
  select_block
    item: x
    from
      table: A
  intersect
    select_block
      item: x
      from
        table: B
    select_block
      item: x
      from
        table: C
```

The parenthesised query is returned as one `query_operand`, so its grouping survives, independently of the left-to-right rule. The same is true for `( condition )` in conditions.

## 6. Parsing Strategy

The parser is a handwritten **top-down predictive recursive-descent parser (LL(1))**:

- the first `L`: read the tokens from left to right;
- the second `L`: build a leftmost derivation;
- `1`: one token of lookahead decides every choice.

No two alternatives start with the same token, so the parser never backtracks:

| Choice | Deciding token |
| --- | --- |
| `statement` | `CREATE` / `INSERT` / `SELECT` or `(` |
| `query_operand` | `SELECT` or `(` |
| `select_list` | `*`, or an item (identifier or aggregate keyword) |
| `select_item` | aggregate keyword (`COUNT` … `MAX`) or identifier |
| `from_list` | after the first table: `,` / `JOIN` / anything else |
| `table_ref` | `(` (subquery) or identifier (base table) |
| base-table alias | `AS`, or an identifier (which can only be an alias, because keywords are reserved) |
| `not_condition` | `NOT` or not |
| `condition_primary` | `(` or an operand |
| `operand` | integer / string / identifier / aggregate keyword |

The grammar has no left recursion. A rule like `Query ::= Query "UNION" Query_operand` cannot be written directly as recursive descent, because `parseQuery()` would call `parseQuery()` again before reading any token, which never ends. Each left-associative level uses an EBNF repetition instead. For the set operators the parser does this:

```
parse_query_operand(result)
while next token is UNION, INTERSECT or EXCEPT:
    op = read the operator (and ALL after UNION)
    result = SetOperation(op, left = result, right = empty)
    parse_query_operand(result.right)
return result
```

Building the node inside a left-to-right `while` loop always makes the tree **left-deep**: in `A UNION B EXCEPT C`, `A UNION B` is finished first and then becomes the left child of `EXCEPT`. The same loop shape is used for `OR` (over `and_condition`) and `AND` (over `not_condition`), and for every comma list.

`NOT` is deliberately right recursive:

```ebnf
not_condition ::= NOT not_condition
               |  condition_primary
```

so repeated `NOT`s nest from the right.

The parser methods mirror the grammar hierarchy directly:

```
parseScript
    -> parseStatement
        -> parseCreate / parseInsert / parseQuery
                                         -> parseQueryOperand
                                             -> parseSelectBlock
                                                 -> parseSelectList -> parseSelectItem
                                                 -> parseFrom -> parseTableRef (a subquery calls parseQuery)
                                                 -> parseClausesAfterFrom
```

Conditions have their own hierarchy:

```
parseOr                 (or_condition)
    -> parseAnd         (and_condition)
        -> parseNot     (not_condition)
            -> parsePrimary   ( "(" condition ")" calls parseOr again )
                -> parseComparison -> parseOperand
```

**Nesting limit.** Every `(`, FROM subquery, `NOT`, `AND`, `OR` and set operator adds one level. More than 1000 levels (`kMaxNestingDepth`, the same budget as the rest of the project) is a syntax error, `nesting too deep`, at the token that goes too deep. Without the limit, very deep input such as 100 000 `(` would overflow the C++ stack and crash, instead of giving an error. To make 1000 levels work, the parser builds each node in place inside its parent (the parent's child list lives on the heap), so each recursive call keeps only a small stack frame.

**The plan must fit too.** The parser counts SQL levels, but common's limit (`limits.h`) is on *plan* levels, and one SQL level can become several: a FROM subquery becomes a `rename` and a `project`. So after translation the finished plan is measured the way common's JSON reader counts it, and a plan over 1000 levels is `syntax: Query nested too deep: its plan has more than 1000 levels.` In practice: 499 nested FROM subqueries are accepted and 500 are refused; 999 chained UNION ALLs are accepted and 1000 are refused. This is a semantic step after parsing; the parse itself still accepts 1000 levels.

## 7. Syntax vs Semantic Validation

The grammar answers only whether the text has the right structure. Meaning is checked after parsing, against the catalog (SPEC §M3.3, sql chunks 07–11).

### Case of

```
SELECT name FROM Emp GROUP BY did

Result: parses. The grammar allows any column in the select list.
The aggregate check then rejects it: `name` is neither a grouping
column nor an aggregate (required case 6, `error: aggregate:`).
```

Examples:

- `SELECT city FROM Emp UNION SELECT budget FROM Dept` is syntactically valid, even if `city` is a VARCHAR and `budget` an INT. The set-operator check then gives a schema error (required case 14).
- `SELECT a FROM R WHERE COUNT(*) > 1` is syntactically valid, because an aggregate is an allowed operand (it is needed in HAVING). The aggregate check rejects it in WHERE (required case 7).
- `SELECT x FROM Nope` is syntactically valid. Name resolution reports the unknown table.

Semantic validation is responsible for:

- unknown tables and unknown columns (with different messages, case 11), and a qualifier that is not in scope;
- ambiguous unqualified columns, naming every candidate (case 3);
- duplicate aliases in one FROM (`FROM Emp, Emp`, case 12);
- the rules for ORDER BY columns (output aliases first, then the FROM scope; sql D3);
- comparisons between different types (case 10), `SUM` / `AVG` on a non-INT column, and `*` in any aggregate other than `COUNT`;
- aggregates in WHERE or ON, and non-grouping columns in the select list or HAVING of an aggregating query (cases 6, 7);
- set operators whose inputs differ in column count or column types (case 14);
- `VARCHAR(n)` with n outside 1..255, and INSERT values that do not match the table (a table that already exists is reported by storage when the shell creates it);
- a plan deeper than 1000 levels (section 6).

Keeping these out of the grammar keeps the grammar small and LL(1), and it lets `sql parse` run without a catalog.

## 8. Required Error Behavior

Every error has one format and no stack trace (§M3.6):

```
error: <category>: <message> at line L, column C
```

The lexer reports `lexical` errors and the parser reports `syntax` errors. Only the first error is reported (panic mode): the first bad token stops the whole parse, so nothing is printed for earlier statements either. A syntax error is reported **at the token the parser could not use**, and the message says what it expected there.

Examples (exact output of `sql parse`):

| Input | Error |
| --- | --- |
| `SELECT FROM R` | `error: syntax: expected a column, an aggregate or '*' but found 'FROM' at line 1, column 8` |
| `SELECT a, FROM R` | `error: syntax: expected a column, an aggregate or '*' but found 'FROM' at line 1, column 11` |
| `SELECT a FROM R WHERE` | `error: syntax: expected an operand but found end of input at line 1, column 22` |
| `SELECT a FROM R WHERE (a = 1` | `error: syntax: expected ')' but found end of input at line 1, column 29` |
| `SELECT a FROM R e JOIN S` | `error: syntax: expected ON but found end of input at line 1, column 25` |
| `SELECT a FROM (SELECT a FROM R)` | `error: syntax: expected AS and an alias (a subquery in FROM needs one) but found end of input at line 1, column 32` |
| `SELECT a FROM R LIMIT -1` | `error: syntax: expected a non-negative integer but found '-1' at line 1, column 23` |
| `CREATE TABLE R (a TEXT)` | `error: syntax: expected INT or VARCHAR but found 'TEXT' at line 1, column 19` |
| `SELECT a FROM R WHERE b = 'abc` | `error: lexical: unterminated string at line 1, column 27` |
| `SELECT a FROM R WHERE a != 1` | `error: lexical: unexpected character '!' at line 1, column 25` |
| `SELECT a FROM R WHERE a > 12abc` | `error: lexical: number '12' runs straight into a letter at line 1, column 27` |

The grammar accepts some statements that the checks after parsing refuse (`sql compile` / `sql check`, `DESIGN_M3.md`). One of them follows from the grammar directly: `select_item ::= ( column | aggregate ) [ AS ident ]` allows the same aggregate twice under two aliases, but a physical plan names both columns the same (`count_star`), and the executor refuses that, so it is a `schema` error:

| Input | Error (exact output of `sql check`) |
| --- | --- |
| `SELECT COUNT(*) AS a, COUNT(*) AS b FROM Emp` | `error: schema: Aggregates 'a' and 'b' are the same aggregate (count_star); select it once. at line 1, column 23` |

## 9. Sanity Checks Against Required Test Cases

The following results are decided by the grammar alone. Each can be reproduced with `sql parse --explain`, and most are also unit tests in `modules/sql/tests/` (parser, condition and explain tests):

| Test | Expected result |
| --- | --- |
| `A UNION B EXCEPT C` | `(A UNION B) EXCEPT C` |
| `A EXCEPT B EXCEPT C` | `(A EXCEPT B) EXCEPT C` |
| `A UNION ALL B INTERSECT C` | `(A UNION ALL B) INTERSECT C` (one level) |
| `A UNION (B EXCEPT C)` | the parentheses keep `B EXCEPT C` together |
| `SELECT city FROM Emp UNION ALL …` against `… UNION …` (example 5.7, case 4) | trees differ in one node: `union_all` against `union` |
| `a = 1 OR b = 2 AND NOT c = 3` | `a = 1 OR (b = 2 AND (NOT c = 3))` |
| `a = 1 AND b = 2 OR c = 3` | `(a = 1 AND b = 2) OR c = 3` |
| `NOT NOT a = 1` | `NOT (NOT a = 1)` |
| `FROM Emp e, Dept d` against `FROM Emp e JOIN Dept d ON …` (examples 5.3 / 5.2, case 15) | comma list against JOIN chain, with the ON condition attached to its JOIN |
| `FROM (SELECT …) AS t` (example 5.6, case 8) | subquery entry with alias `t`; without `AS t`: syntax error |
| `HAVING COUNT(*) > 5` (example 5.4, case 5) | aggregate parsed as an operand |
| `SELECT a FROM R WHERE a < b < c` | syntax error at the second `<` |
| 1000 nested `(`, `NOT`s or subqueries | parsed; 1001 → `nesting too deep` (the translator then also checks the plan's depth, section 6) |

The tokenizer rules also decide these cases:

- `age>=30` → `>=` is one token; `a<>b` → `<>` is one token.
- `age>-30` → `>` and `-30`; `age > - 30` → lexical error at `-`.
- `name = 'a,b)'` → `,` and `)` are part of the string.
- `name = 'O''Brien'` → the doubled quote is one literal quote.
- `SeLeCt a FrOm R` → keywords in any case; `Emp` and `emp` stay different identifiers.
- `SELECT a FROM R -- comment` → the comment produces no tokens.
- `SELECT a FROM R; SELECT b FROM S;` → two statements.

## 10. Where This Grammar Differs from the §M3.2 Sketch, and Why

§M3.9 asks for the places where this grammar differs from the §M3.2 sketch and why. Renaming rules (for example `query_operand` for the sketch's `"(" query ")"` alternative, or the `comma_tail` / `join_tail` split of `from_list`) does not change the language and is not listed.

| # | Difference | Why |
| --- | --- | --- |
| D1 | **`AS` is optional before a base table's alias** (`Emp e`). It stays required for a subquery alias and for a select-item alias. | The spec's own examples write `FROM Emp e JOIN Dept d` (5.2), `FROM Emp e, Dept d` (5.3) and `FROM Emp e JOIN Emp m` (5.5); the sketch's `ident [ AS ident ]` would reject all three. Because keywords are reserved (D5), an identifier right after a table name can only be an alias, so one token still decides. A select-item alias keeps `AS` so that `SELECT name n` is reported as an error instead of being guessed. |
| D2 | **ASC / DESC per ORDER BY item** (`sort_item ::= column [ ASC \| DESC ]`). | The handout writes `ORDER BY cols [ASC|DESC]`, one direction for the whole list. The logical `sort` node has a direction per key (`[["n","desc"], …]`, §M3.5), and `ORDER BY a DESC, b` is ordinary SQL. ASC is the default. |
| D3 | **The final `;` is optional, and empty statements are skipped** (`script ::= [ statement ] { ";" [ statement ] } EOF`). | §M3.1 says statements are *separated* by semicolons, so a separator after the last statement is not needed. The shell and `sql parse --query` usually pass one statement without `;`. |
| D4 | **An aggregate is an allowed operand in a condition** (`operand ::= column \| literal \| aggregate`). | The sketch's `operand ::= column \| int \| string` cannot express `HAVING COUNT(*) > 5` (example 5.4). An aggregate in WHERE or ON is then rejected by the aggregate check with an `aggregate` error (required case 7), which is a clearer message than a syntax error. |
| D5 | **Every keyword is reserved**, including `COUNT` … `MAX`, `INT` and `VARCHAR`. | The sketch does not say. With reserved words, one token always decides: in `FROM Emp e`, `e` must be an alias, and in `FROM Emp WHERE …`, `WHERE` cannot be one. The cost is that no table or column can be called `count` or `order`; PostgreSQL allows some of these words as names. |
| D6 | **`LIMIT` takes an integer of value 0 or more.** A negative count is a syntax error at the number; `LIMIT 0` is allowed and returns no rows. | The sketch writes `LIMIT int`, and §M3.1 integers are signed. A negative row count has no meaning, so it is rejected where it is written. |
| D7 | **`!=` is a lexical error**; only `<>` means "not equal". | §M3.1 / §M3.2 list `<>` and no `!=`. Bonus 1's relational-algebra language used `!=`, so this is a deliberate change for SQL. |
| D8 | **A sign must touch its digits** (`-5`, never `- 5`), and a lone `+` or `-` is a lexical error. | The subset has no arithmetic, so a free-standing sign can only be a typing mistake. |
| D9 | **A nesting limit of 1000 levels** (each `(`, FROM subquery, `NOT`, `AND`, `OR` and set operator is one level); deeper input gives `syntax: nesting too deep`. | The sketch has no limit. Recursive descent uses one C++ stack frame per level, so without a limit very deep input would crash the program. 1000 is the project-wide `kMaxNestingDepth` (DECISIONS "Nesting limit"). Because one SQL level can become several plan levels, the translator also checks the finished plan against the same limit (section 6), so the plan JSON that opt and exec read is always within it. |
| D10 | **`ORDER BY` and `LIMIT` belong to each select block, never to a whole set operation.** | This is what the sketch's `select_block` rule says, and it is kept unchanged. It is listed because it differs from standard SQL and PostgreSQL, where a trailing ORDER BY sorts the whole union (section 3.5 shows how to write that here). |

Sources cited inside the main Design Document of Module 3