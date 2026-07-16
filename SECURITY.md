# Security policy

Report vulnerabilities privately through GitHub's security-advisory interface for the project.
Do not include private corpora, answers, or database files in a public report.

Supported releases are the latest tagged release and the current default branch. The CLI never
executes user-supplied SQL or accesses the network. Treat corpus Markdown, manifest JSON, bank
JSON, and restored databases as untrusted input. Keep backups outside publicly synchronized
directories when they contain learning records.

Restore validates application identity, interface and schema versions, SQLite integrity, and
foreign keys before replacing the destination. The `--yes` flag is required because replacement
is destructive.
