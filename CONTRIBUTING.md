# Contributing

Changes must preserve the fixed JSON CLI contract, schema invariants, and strict C23 build.

1. Configure a release build and run CTest.
2. Configure a debug build with `LBDB_ENABLE_SANITIZERS=ON` and run CTest again.
3. Build with both GCC and Clang when changing portable code.
4. Run `clang-format --dry-run --Werror` when `clang-format` is available.
5. Do not add arbitrary SQL execution, hidden network access, vendored dependencies, or a second
   JSON implementation. JSON parsing remains delegated to SQLite's checked runtime functions.
6. Add a C unit or integration test for every behavior change.

Prepared statements are required for runtime values. New writes must use `BEGIN IMMEDIATE` and
record one `db_operations` entry in the transaction. Quiz state changes must also record a
`quiz_events` entry.
