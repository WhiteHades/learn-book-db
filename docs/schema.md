# Schema version 1

The database identifies itself with:

- `metadata.schema_version = 1`
- `metadata.db_interface_name = learn-book-db`
- `metadata.db_interface_version = 1`
- `PRAGMA user_version = 1`

There is no migration or legacy-compatibility layer. `schema_migrations` records the one fresh
schema installation and its SHA-256 checksum. Health checks also fingerprint every ordered,
non-internal `sqlite_schema` definition against a fresh in-memory schema, so missing or altered
tables, indexes, triggers, and views are rejected.

## Source and bank

`source_units` normalizes manifest units and stores the source SHA-256. `source_sections` stores
line/page bounds parsed from page markers and numbered or summary headings. Source changes are
accepted only while a unit has no imported concepts or questions.

`concepts` and `concept_sources` define learning objectives and one or more ordered body passages.
`question_bank` and `question_sources` hold canonical questions, exact rubrics, ordered source
links, revisions, and retirement state. A summary section may never be the sole or partial source
for a concept or question. Flat provenance fields must match the canonical source links.
Multiple-choice expected answers must equal exactly one option.

`tags`, `tag_aliases`, `tag_relations`, and `question_tags` normalize extensible classification.
Every active question must have `topic`, `theme`, and `mode` tags before validation succeeds.

## Templates and immutable snapshots

`quiz_templates` and `quiz_template_questions` materialize cumulative 25%, 50%, 75%, and final
chapter selections plus topic/theme selections. The latter prioritize unseen questions, then weak
latest assessments.

`quiz_sessions`, `quiz_objectives`, and `quiz_questions` are snapshots. Session states are
`planned`, `in_progress`, `paused`, `completed`, and `abandoned`. Question states are `planned`,
`asked`, `deferred`, `answered`, and `retired`. New sessions are assembled in `planned`, then
transition to `in_progress` in the same transaction with both events recorded. Abandoning a session
retires every unresolved question before making the session terminal.

`quiz_responses` stores numbered attempts. Progress and mastery use only the greatest attempt
number for each question. Regrading changes one selected response but records before/after values
in both audit surfaces. Completed and abandoned session objectives, questions, and responses reject
inserts, updates, and deletes through triggers.

## Evidence and audit

`learning_records` is chronological evidence with states `learning`, `review`, `mastered`, and
`needs_correction`. Optional foreign keys link evidence to a quiz and response.

Every database mutation appends one `db_operations` row in the same transaction. Quiz lifecycle,
question lifecycle, answer, follow-up, and regrade changes additionally append `quiz_events`.

## Views

- `quiz_progress`: state counts and latest-attempt assessment counts for each session.
- `source_coverage`: concepts and question-format counts for each source unit.
- `tag_catalog`: normalized tags and their active/total question counts.

Indexes cover unit ordering, source links, bank checkpoint selection, tag membership, template
membership, lifecycle status, response attempts, evidence topics, operations, and events.
