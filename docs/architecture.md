# Architecture

## Boundaries

The executable is a thin owner of `LbdbApp`. Its static support library and opaque-handle headers are
private build components; installation exposes only the CLI. Constructors return owned handles and
matching destructors release them. This is object-oriented C through explicit ownership and
encapsulation, not class emulation.

The layers are:

1. `app`, `command`: global-option parsing, binary command lookup, output/error routing.
2. `commands_*`: fixed command handlers and domain transitions.
3. `database`, `statement`: SQLite ownership, prepared binding, transactions, backup, and runtime
   JSON capability checks.
4. `json`, `path`, `sha256`: allocation-checked JSON output, bounded filesystem operations, and
   source hashing.
5. `schema`: the sole fresh schema-v1 definition.

There is no mutable global state. Command and lifecycle registries are immutable sorted tables.

## Transactions and concurrency

Read commands open `file:` URIs with `mode=ro` and set `query_only=ON`. Write commands enable WAL,
foreign keys, a 5-second busy timeout, and use `BEGIN IMMEDIATE`. Domain rows, audit rows, and quiz
events commit or roll back together. SQLite's backup API provides consistent backups without
blocking readers for the duration of a file copy.

## Lifecycle tables

Session and question transitions are looked up in explicit sorted transition tables. A handler
cannot write a target state until the corresponding `(event, from, to)` tuple is present. Terminal
session snapshots reject objective, question, and response changes. Bank revisions affect only
future snapshots.

## Complexity

- Command dispatch is `O(log C)` over `C` fixed commands.
- State-transition lookup is `O(log T)` over a small immutable transition table.
- Whole-file reading, SHA-256 hashing, and Markdown section parsing are `O(B)` in source bytes.
- Corpus synchronization is `O(B + S)` per unit, plus indexed SQLite writes for `S` sections.
- Question and template lookups use B-tree indexes and are `O(log N + K)` for `K` returned rows.
- JSON construction uses a contiguous growable buffer, amortized `O(1)` append and `O(B)` total.
- Topic/theme quiz selection scans its indexed template membership and sorts by weakness/history;
  it is `O(K log K)` in candidate count. Chapter snapshots preserve stored template order.

Vectors are used only where data must be retained across a parse or transaction. Streaming SQLite
statements are preferred elsewhere.
