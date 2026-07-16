# learn-book-db

`learn-book-db` is a deterministic C23 CLI for source-grounded question banks,
immutable quiz snapshots, assessed responses, and chronological learning evidence. It stores
data in SQLite and exposes only a fixed command registry: there is no arbitrary SQL command.

## Requirements

- A C23 compiler with language extensions disabled
- CMake 3.25 or newer
- SQLite 3 with JSON functions available at runtime
- POSIX.1-2008 for filesystem and integration-test process APIs

SQLite is the only runtime library dependency.

## Build and test

```sh
git clone https://github.com/WhiteHades/learn-book-db
cd learn-book-db
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For an AddressSanitizer and UndefinedBehaviorSanitizer build:

```sh
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLBDB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Install and uninstall

```sh
cmake --install build --prefix "$HOME/.local"
```

The install contains the `learn-book-db` executable, documentation, and exchange-format examples.
The implementation headers and static support library are private build artifacts.

The build records every installed file. To remove exactly those files, retain the build tree
and run:

```sh
cmake --build build --target uninstall
```

For staged packaging, use `DESTDIR` with `cmake --install`. Do not run remote scripts through a
shell.

## Global interface

```text
learn-book-db [--root PATH] [--db PATH] [--manifest PATH] [--pretty]
              GROUP COMMAND [OPTIONS]
learn-book-db --version
learn-book-db --help
learn-book-db --help GROUP COMMAND
```

Defaults are the current directory, `.book-learning/learning.db`, and
`.book-learning/manifest.json`. Relative paths are resolved from `--root`.

Every successful command writes one JSON object to standard output. Every failure writes one
JSON error object to standard error and exits nonzero. Mutations use `BEGIN IMMEDIATE`, append a
`db_operations` row in the same transaction, and append `quiz_events` for quiz transitions.

## Commands

```text
db init|doctor|status|backup PATH|restore PATH --yes
corpus sync|status
bank import FILE...|export --unit REF --output PATH
bank export-all --output-dir DIR|validate [--allow-incomplete]
bank search [filters]|show ID|revise ID --input JSON_OR_@PATH
bank retire ID --reason TEXT|activate ID
tag list|alias-add|alias-remove|relation-add|relation-remove
template rebuild|list|show ID
quiz start --template ID [--limit N]|list|status ID|next ID
quiz defer ID --question ID --reason TEXT|requeue ID --question ID
quiz follow-up ID --input JSON_OR_@PATH|pause ID [--reason]|resume ID
quiz complete ID|abandon ID --reason TEXT
response submit --question ID --answer TEXT --assessment ASSESSMENT --feedback TEXT
response regrade --response ID --assessment ASSESSMENT --feedback TEXT --reason TEXT
learning add|list|show ID
report active|quiz ID|coverage|mastery|state-drift
stats
```

Run `learn-book-db --help` for the command list and `learn-book-db --help GROUP COMMAND` for one
command's exact usage. See
[`docs/agent-interface.md`](docs/agent-interface.md) for machine-facing behavior and
[`docs/schema.md`](docs/schema.md) for invariants.

## Quick start

Copy `examples/manifest.json` to `.book-learning/manifest.json`, adjust source paths, then:

```sh
learn-book-db db init
learn-book-db corpus sync
learn-book-db bank import examples/bank.json
learn-book-db template rebuild
learn-book-db bank validate
learn-book-db template list --active active
```

The example source and bank are intentionally small. They demonstrate the exchange format, not
a complete curriculum. Manifests and banks must declare `"format_version": 1`; unsupported or
missing versions are rejected.
