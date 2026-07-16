#ifndef LBDB_INTERNAL_H
#define LBDB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "learn_book_db/learn_book_db.h"

#define LBDB_DEFAULT_DB ".book-learning/learning.db"
#define LBDB_DEFAULT_MANIFEST ".book-learning/manifest.json"

#if defined(__GNUC__) || defined(__clang__)
#define LBDB_PRINTF_LIKE(format_index, argument_index)                                             \
    __attribute__((format(printf, format_index, argument_index)))
#else
#define LBDB_PRINTF_LIKE(format_index, argument_index)
#endif

typedef LbdbError (*LbdbCommandHandler)(LbdbCommand *command);

typedef struct LbdbCommandEntry {
    const char *key;
    LbdbCommandHandler handler;
    const char *summary;
    const char *usage;
} LbdbCommandEntry;

typedef struct LbdbStringVector {
    char **items;
    size_t count;
    size_t capacity;
} LbdbStringVector;

typedef struct LbdbArgs {
    LbdbApp *app;
    int argc;
    char **argv;
    bool *used;
} LbdbArgs;

struct LbdbApp {
    int argc;
    char **argv;
    char *root;
    char *database_path;
    char *manifest_path;
    bool pretty;
    bool help_requested;
    bool version_requested;
    int command_start;
    LbdbJsonWriter *output;
    LbdbCommand *command;
    LbdbError error;
    char *error_message;
    char *error_details;
};

struct LbdbCommand {
    LbdbApp *app;
    const LbdbCommandEntry *entry;
    char *key;
    int argc;
    char **argv;
};

char *lbdb_string_duplicate(const char *value);
char *lbdb_string_format(const char *format, ...) LBDB_PRINTF_LIKE(1, 2);
LbdbError lbdb_string_vector_push(LbdbStringVector *vector, const char *value);
void lbdb_string_vector_destroy(LbdbStringVector *vector);

LbdbError lbdb_app_fail(LbdbApp *app, LbdbError error, const char *format, ...)
    LBDB_PRINTF_LIKE(3, 4);
LbdbError lbdb_app_fail_details(LbdbApp *app, LbdbError error, const char *details_json,
                                const char *format, ...) LBDB_PRINTF_LIKE(4, 5);
LbdbError lbdb_app_database_error(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                                  const char *operation);
void *lbdb_database_native_handle(LbdbDatabase *database);
LbdbError lbdb_app_open_database(LbdbApp *app, LbdbDatabaseMode mode, LbdbDatabase **out_database);
LbdbError lbdb_output_begin(LbdbApp *app, const char *command_name);
LbdbError lbdb_output_end(LbdbApp *app);
LbdbError lbdb_output_statement_rows(LbdbApp *app, LbdbStatement *statement);

LbdbError lbdb_args_init(LbdbArgs *args, LbdbCommand *command);
void lbdb_args_destroy(LbdbArgs *args);
LbdbError lbdb_args_option(LbdbArgs *args, const char *name, bool required, const char **value);
LbdbError lbdb_args_flag(LbdbArgs *args, const char *name, bool *present);
LbdbError lbdb_args_positionals(LbdbArgs *args, LbdbStringVector *values);
LbdbError lbdb_args_finish(LbdbArgs *args);
LbdbError lbdb_parse_positive_int(LbdbApp *app, const char *name, const char *value,
                                  int64_t *result);
bool lbdb_string_in_set(const char *value, const char *const values[], size_t count);

LbdbError lbdb_resolve_path(LbdbApp *app, const char *value, bool must_exist,
                            bool require_within_root, char **resolved);
LbdbError lbdb_make_parent_directories(LbdbApp *app, const char *path);
LbdbError lbdb_read_file(LbdbApp *app, const char *path, char **contents, size_t *size);
LbdbError lbdb_write_file_exclusive(LbdbApp *app, const char *path, const void *contents,
                                    size_t size);
LbdbError lbdb_write_json_file_exclusive(LbdbApp *app, const char *path,
                                         const LbdbJsonWriter *writer);
LbdbError lbdb_file_sha256(LbdbApp *app, const char *path, char output[65]);
bool lbdb_path_exists(const char *path);
bool lbdb_path_is_file(const char *path);

LbdbError lbdb_begin_write(LbdbApp *app, LbdbDatabase **database);
LbdbError lbdb_commit_write(LbdbApp *app, LbdbDatabase *database, const char *command,
                            const char *entity_type, int64_t entity_id, const char *details_json);
LbdbError lbdb_record_operation(LbdbApp *app, LbdbDatabase *database, const char *command,
                                const char *entity_type, int64_t entity_id,
                                const char *details_json);
LbdbError lbdb_add_quiz_event(LbdbApp *app, LbdbDatabase *database, int64_t quiz_id,
                              int64_t question_id, int64_t response_id, const char *event_type,
                              const char *reason, const char *payload_json);

LbdbError lbdb_json_document_type(LbdbApp *app, LbdbDatabase *database, const char *json,
                                  const char *expected_type);
LbdbError lbdb_json_get_text(LbdbApp *app, LbdbDatabase *database, const char *json,
                             const char *path, bool required, char **value);
LbdbError lbdb_json_get_raw(LbdbApp *app, LbdbDatabase *database, const char *json,
                            const char *path, bool required, char **value);
LbdbError lbdb_json_get_int(LbdbApp *app, LbdbDatabase *database, const char *json,
                            const char *path, bool required, int64_t *value, bool *present);
LbdbError lbdb_json_get_bool(LbdbApp *app, LbdbDatabase *database, const char *json,
                             const char *path, bool required, bool *value, bool *present);
LbdbError lbdb_json_array_size(LbdbApp *app, LbdbDatabase *database, const char *json,
                               const char *path, size_t *size);
LbdbError lbdb_resolve_unit_id(LbdbApp *app, LbdbDatabase *database, const char *reference,
                               int64_t *unit_id);
LbdbError lbdb_resolve_tag_id(LbdbApp *app, LbdbDatabase *database, const char *reference,
                              int64_t *tag_id);
LbdbError lbdb_insert_learning_record(LbdbApp *app, LbdbDatabase *database, const char *topic,
                                      const char *state, const char *evidence,
                                      const char *next_step, const char *source_type,
                                      int64_t quiz_id, int64_t response_id, int64_t *record_id);
LbdbError lbdb_render_quiz_progress(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                    int64_t quiz_id);
LbdbError lbdb_render_session_summary(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                      LbdbStatement *session);
LbdbError lbdb_render_public_question(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                      int64_t question_id);

const char *lbdb_schema_sql(void);
size_t lbdb_schema_sql_size(void);
const char *const *lbdb_required_tables(size_t *count);
const char *const *lbdb_required_views(size_t *count);

LbdbError lbdb_command_db_init(LbdbCommand *command);
LbdbError lbdb_command_db_doctor(LbdbCommand *command);
LbdbError lbdb_command_db_status(LbdbCommand *command);
LbdbError lbdb_command_db_backup(LbdbCommand *command);
LbdbError lbdb_command_db_restore(LbdbCommand *command);
LbdbError lbdb_command_corpus_sync(LbdbCommand *command);
LbdbError lbdb_command_corpus_status(LbdbCommand *command);
LbdbError lbdb_command_bank_import(LbdbCommand *command);
LbdbError lbdb_command_bank_export(LbdbCommand *command);
LbdbError lbdb_command_bank_export_all(LbdbCommand *command);
LbdbError lbdb_command_bank_validate(LbdbCommand *command);
LbdbError lbdb_command_bank_search(LbdbCommand *command);
LbdbError lbdb_command_bank_show(LbdbCommand *command);
LbdbError lbdb_command_bank_revise(LbdbCommand *command);
LbdbError lbdb_command_bank_retire(LbdbCommand *command);
LbdbError lbdb_command_bank_activate(LbdbCommand *command);
LbdbError lbdb_command_tag_list(LbdbCommand *command);
LbdbError lbdb_command_tag_alias_add(LbdbCommand *command);
LbdbError lbdb_command_tag_alias_remove(LbdbCommand *command);
LbdbError lbdb_command_tag_relation_add(LbdbCommand *command);
LbdbError lbdb_command_tag_relation_remove(LbdbCommand *command);
LbdbError lbdb_command_template_rebuild(LbdbCommand *command);
LbdbError lbdb_command_template_list(LbdbCommand *command);
LbdbError lbdb_command_template_show(LbdbCommand *command);
LbdbError lbdb_command_quiz_start(LbdbCommand *command);
LbdbError lbdb_command_quiz_list(LbdbCommand *command);
LbdbError lbdb_command_quiz_status(LbdbCommand *command);
LbdbError lbdb_command_quiz_next(LbdbCommand *command);
LbdbError lbdb_command_quiz_defer(LbdbCommand *command);
LbdbError lbdb_command_quiz_requeue(LbdbCommand *command);
LbdbError lbdb_command_quiz_follow_up(LbdbCommand *command);
LbdbError lbdb_command_quiz_pause(LbdbCommand *command);
LbdbError lbdb_command_quiz_resume(LbdbCommand *command);
LbdbError lbdb_command_quiz_complete(LbdbCommand *command);
LbdbError lbdb_command_quiz_abandon(LbdbCommand *command);
LbdbError lbdb_command_response_submit(LbdbCommand *command);
LbdbError lbdb_command_response_regrade(LbdbCommand *command);
LbdbError lbdb_command_learning_add(LbdbCommand *command);
LbdbError lbdb_command_learning_list(LbdbCommand *command);
LbdbError lbdb_command_learning_show(LbdbCommand *command);
LbdbError lbdb_command_report_active(LbdbCommand *command);
LbdbError lbdb_command_report_quiz(LbdbCommand *command);
LbdbError lbdb_command_report_coverage(LbdbCommand *command);
LbdbError lbdb_command_report_mastery(LbdbCommand *command);
LbdbError lbdb_command_report_state_drift(LbdbCommand *command);
LbdbError lbdb_command_stats(LbdbCommand *command);
LbdbError lbdb_command_write_help(LbdbApp *app, int argc, char *const argv[]);

#define LBDB_TRY(expression)                                                                       \
    do {                                                                                           \
        LbdbError lbdb_try_error__ = (expression);                                                 \
        if (lbdb_try_error__ != LBDB_OK) {                                                         \
            return lbdb_try_error__;                                                               \
        }                                                                                          \
    } while (false)

#define LBDB_JSON(app, expression)                                                                 \
    do {                                                                                           \
        if ((app)->error == LBDB_OK && !(expression)) {                                            \
            (void)lbdb_app_fail((app), LBDB_ERROR_MEMORY, "Unable to build JSON output");          \
        }                                                                                          \
    } while (false)

#endif
