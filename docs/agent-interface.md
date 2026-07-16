# Agent interface

## Output contract

Each invocation emits exactly one JSON object on one stream:

- success: standard output, `{"ok":true,"command":"group.action",...}`
- failure: standard error,
  `{"ok":false,"error":{"code":"validation","message":"...","details":...}}`

`--pretty` changes whitespace only. IDs are SQLite integer primary keys. Timestamps are UTC text in
`YYYY-MM-DDTHH:MM:SS.sssZ` form.

Agents must inspect the process exit status before consuming standard output. They must not infer
success from an empty error stream.

`learn-book-db --help` returns every command with its exact usage string.
`learn-book-db --help GROUP COMMAND` returns deterministic help for one command without opening a
database.

## Safe references

Unit references accept an integer ID, a globally unique unit key, or `corpus-slug/unit-key`. Tag
references accept an integer ID, `kind:name`, a globally unique name, or an alias. Ambiguous short
references fail and return canonical `{id,reference}` alternatives in
`error.details.alternatives`.

`JSON_OR_@PATH` accepts an inline JSON object or an `@` followed by a path. Relative paths resolve
under `--root`. Input JSON is parsed with SQLite JSON functions only after runtime capability
checks.

Corpus manifests and bank files must contain integer `format_version` with value `1`. Missing
versions are validation errors; other versions are unsupported. Follow-up objects accept only the
canonical `question_type` and `response_format` fields.

## Answer secrecy

`quiz next`, `quiz status`, `quiz list`, `report active`, and template output never include an
expected answer, grading criteria, or answer justification. `response submit --reveal-answer`
reveals the expected answer and rubric only after the response and any linked evidence commit.
Administrative `bank show` and exports include those fields intentionally.

## Idempotence and conflicts

- `db init` refuses an existing destination.
- `quiz start` returns an existing active session for the same template.
- Duplicate aliases, relations, and existing unit banks are conflicts.
- Exports and backups refuse to overwrite files.
- `db restore` requires `--yes` and validates the staged database before replacement.

## Full filters

`bank search` accepts `--unit REF`, repeated `--tag REF`, `--tag-kind KIND`,
`--question-type TYPE`, `--response-format FORMAT`, `--active active|retired|all`, `--text TEXT`,
and `--limit N`.

`tag list` accepts `--kind KIND` and `--search TEXT`. `template list` accepts `--scope SCOPE`,
`--unit REF`, `--tag REF`, and `--active active|inactive|all`. `quiz list` accepts `--status STATE`
and `--scope SCOPE`. `learning list` accepts `--status STATUS`, `--topic TEXT`, `--quiz ID`, and
`--limit N`.

## Linked learning evidence

`response submit` optionally accepts all of `--learning-topic`, `--learning-status`, and
`--evidence`, plus optional `--next-step`. The response, question state, event, operation, and
learning record commit atomically.

`learning add` requires `--topic`, `--status`, and `--evidence`; it also accepts `--next-step`,
`--source-type conversation|quiz|code_review`, `--quiz ID`, and `--response ID`. Quiz-linked
evidence must use source type `quiz`.
