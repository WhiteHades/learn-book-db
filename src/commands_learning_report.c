#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *const valid_learning_states[] = {"learning", "mastered", "needs_correction",
                                                    "review"};
static const char *const valid_source_types[] = {"code_review", "conversation", "quiz"};

static bool text_exists(const char *value) {
    if (value == NULL) {
        return false;
    }
    for (; *value != '\0'; ++value) {
        if (!isspace((unsigned char)*value)) {
            return true;
        }
    }
    return false;
}

static LbdbError query_error(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                             const char *operation) {
    return error == LBDB_OK ? LBDB_OK : lbdb_app_database_error(app, database, error, operation);
}

LbdbError lbdb_insert_learning_record(LbdbApp *app, LbdbDatabase *database, const char *topic,
                                      const char *state, const char *evidence,
                                      const char *next_step, const char *source_type,
                                      int64_t quiz_id, int64_t response_id, int64_t *record_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t derived_quiz_id = 0;
    LbdbError error = LBDB_OK;
    if (!text_exists(topic) || !text_exists(evidence)) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                             "Learning topic and evidence must not be empty");
    }
    if (!lbdb_string_in_set(state, valid_learning_states,
                            sizeof(valid_learning_states) / sizeof(valid_learning_states[0]))) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Invalid learning state: %s", state);
    }
    if (!lbdb_string_in_set(source_type, valid_source_types,
                            sizeof(valid_source_types) / sizeof(valid_source_types[0]))) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Invalid learning source type: %s",
                             source_type);
    }
    if (response_id > 0) {
        error = lbdb_statement_prepare(
            database,
            "SELECT q.quiz_id FROM quiz_responses r JOIN quiz_questions q ON q.id=r.question_id "
            "WHERE r.id=?1",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, response_id);
        }
        if (error == LBDB_OK) {
            error = query_error(app, database, lbdb_statement_step(statement, &has_row),
                                "Cannot validate learning response link");
        }
        if (error == LBDB_OK && !has_row) {
            error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz response: %lld",
                                  (long long)response_id);
        }
        if (error == LBDB_OK) {
            derived_quiz_id = lbdb_statement_column_int64(statement, 0);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK && quiz_id > 0) {
        error =
            lbdb_statement_prepare(database, "SELECT 1 FROM quiz_sessions WHERE id=?1", &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = query_error(app, database, lbdb_statement_step(statement, &has_row),
                                "Cannot validate learning quiz link");
        }
        if (error == LBDB_OK && !has_row) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz: %lld", (long long)quiz_id);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK && derived_quiz_id > 0 && quiz_id > 0 && derived_quiz_id != quiz_id) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Response %lld does not belong to quiz %lld",
                          (long long)response_id, (long long)quiz_id);
    }
    if (quiz_id == 0) {
        quiz_id = derived_quiz_id;
    }
    if (error == LBDB_OK && strcmp(source_type, "quiz") == 0 && quiz_id == 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Quiz learning evidence requires a quiz or response link");
    }
    if (error == LBDB_OK && strcmp(source_type, "quiz") != 0 && (quiz_id > 0 || response_id > 0)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Quiz-linked learning evidence must use source_type=quiz");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO learning_records(topic,state,evidence,next_step,source_type,quiz_id,"
            "quiz_response_id,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,?7,'{}')",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, topic);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, state);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 3, evidence);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 4, next_step);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 5, source_type);
    }
    if (error == LBDB_OK) {
        error = quiz_id > 0 ? lbdb_statement_bind_int64(statement, 6, quiz_id)
                            : lbdb_statement_bind_null(statement, 6);
    }
    if (error == LBDB_OK) {
        error = response_id > 0 ? lbdb_statement_bind_int64(statement, 7, response_id)
                                : lbdb_statement_bind_null(statement, 7);
    }
    if (error == LBDB_OK) {
        error = query_error(app, database, lbdb_statement_step(statement, NULL),
                            "Cannot insert learning record");
    }
    if (error == LBDB_OK) {
        *record_id = lbdb_statement_last_insert_id(statement);
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError write_learning_record(LbdbApp *app, LbdbJsonWriter *writer,
                                       LbdbStatement *statement) {
    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
        !lbdb_json_key(writer, "created_at") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
        !lbdb_json_key(writer, "topic") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
        !lbdb_json_key(writer, "state") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 3)) ||
        !lbdb_json_key(writer, "evidence") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 4)) ||
        !lbdb_json_key(writer, "next_step") ||
        !lbdb_json_string_or_null(writer, lbdb_statement_column_text(statement, 5)) ||
        !lbdb_json_key(writer, "source_type") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 6)) ||
        !lbdb_json_key(writer, "quiz_id")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
    }
    if (lbdb_statement_column_is_null(statement, 7)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
        }
    } else if (!lbdb_json_int(writer, lbdb_statement_column_int64(statement, 7))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
    }
    if (!lbdb_json_key(writer, "quiz_response_id")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
    }
    if (lbdb_statement_column_is_null(statement, 8)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
        }
    } else if (!lbdb_json_int(writer, lbdb_statement_column_int64(statement, 8))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning record");
    }
    return lbdb_json_end_object(writer)
               ? LBDB_OK
               : lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize learning record");
}

static LbdbError learning_record_query(LbdbApp *app, LbdbDatabase *database, int64_t record_id,
                                       LbdbStatement **statement) {
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id,created_at,topic,state,evidence,next_step,source_type,quiz_id,"
        "quiz_response_id FROM learning_records WHERE id=?1",
        statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(*statement, 1, record_id);
    }
    if (error == LBDB_OK) {
        error = query_error(app, database, lbdb_statement_step(*statement, &has_row),
                            "Cannot load learning record");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown learning record: %lld",
                              (long long)record_id);
    }
    return error;
}

LbdbError lbdb_command_learning_add(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *topic = NULL;
    const char *state = NULL;
    const char *evidence = NULL;
    const char *next_step = NULL;
    const char *source_type = NULL;
    const char *quiz_text = NULL;
    const char *response_text = NULL;
    int64_t quiz_id = 0;
    int64_t response_id = 0;
    int64_t record_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *record = NULL;
    LbdbError error = lbdb_args_init(&args, command);
#define LEARNING_OPTION(name, target, required)                                                    \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_args_option(&args, (name), (required), &(target));                        \
        }                                                                                          \
    } while (false)
    LEARNING_OPTION("--topic", topic, true);
    LEARNING_OPTION("--status", state, true);
    LEARNING_OPTION("--evidence", evidence, true);
    LEARNING_OPTION("--next-step", next_step, false);
    LEARNING_OPTION("--source-type", source_type, false);
    LEARNING_OPTION("--quiz", quiz_text, false);
    LEARNING_OPTION("--response", response_text, false);
#undef LEARNING_OPTION
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && quiz_text != NULL) {
        error = lbdb_parse_positive_int(app, "--quiz", quiz_text, &quiz_id);
    }
    if (error == LBDB_OK && response_text != NULL) {
        error = lbdb_parse_positive_int(app, "--response", response_text, &response_id);
    }
    if (source_type == NULL) {
        source_type = quiz_id > 0 || response_id > 0 ? "quiz" : "conversation";
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_insert_learning_record(app, database, topic, state, evidence, next_step,
                                            source_type, quiz_id, response_id, &record_id);
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "topic") || !lbdb_json_string(details, topic) ||
            !lbdb_json_key(details, "state") || !lbdb_json_string(details, state) ||
            !lbdb_json_key(details, "quiz_id") ||
            (quiz_id > 0 ? !lbdb_json_int(details, quiz_id) : !lbdb_json_null(details)) ||
            !lbdb_json_key(details, "response_id") ||
            (response_id > 0 ? !lbdb_json_int(details, response_id) : !lbdb_json_null(details)) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build learning audit");
        } else {
            error = lbdb_commit_write(app, database, "learning.add", "learning_record", record_id,
                                      lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = learning_record_query(app, database, record_id, &record);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "learning.add");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "record"));
        error = write_learning_record(app, app->output, record);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(record);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_learning_list(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *state = NULL;
    const char *topic = NULL;
    const char *quiz_text = NULL;
    const char *limit_text = NULL;
    char *topic_pattern = NULL;
    int64_t quiz_id = 0;
    int64_t limit = 100;
    int64_t count = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--status", false, &state);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--topic", false, &topic);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--quiz", false, &quiz_text);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--limit", false, &limit_text);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && state != NULL &&
        !lbdb_string_in_set(state, valid_learning_states,
                            sizeof(valid_learning_states) / sizeof(valid_learning_states[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Invalid learning state: %s", state);
    }
    if (error == LBDB_OK && quiz_text != NULL) {
        error = lbdb_parse_positive_int(app, "--quiz", quiz_text, &quiz_id);
    }
    if (error == LBDB_OK && limit_text != NULL) {
        error = lbdb_parse_positive_int(app, "--limit", limit_text, &limit);
    }
    if (error == LBDB_OK && topic != NULL) {
        topic_pattern = lbdb_string_format("%%%s%%", topic);
        if (topic_pattern == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate topic filter");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,created_at,topic,state,evidence,next_step,source_type,quiz_id,"
            "quiz_response_id FROM learning_records WHERE (?1 IS NULL OR state=?1) "
            "AND (?2 IS NULL OR topic LIKE ?2) AND (?3=0 OR quiz_id=?3) ORDER BY id DESC LIMIT ?4",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, state);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, topic_pattern);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 3, quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 4, limit);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "learning.list");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "records"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = write_learning_record(app, app->output, statement);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list learning records");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        error = lbdb_output_end(app);
    }
    free(topic_pattern);
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_learning_show(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbStringVector values = {0};
    int64_t record_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *record = NULL;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_positionals(&args, &values);
    }
    if (error == LBDB_OK && values.count != 1U) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_USAGE, "learning show requires exactly one record ID");
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(app, "record ID", values.items[0], &record_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = learning_record_query(app, database, record_id, &record);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "learning.show");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "record"));
        error = write_learning_record(app, app->output, record);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(record);
    lbdb_database_close(database);
    lbdb_string_vector_destroy(&values);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError no_command_arguments(LbdbCommand *command) {
    LbdbArgs args = {0};
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_report_active(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *sessions = NULL;
    bool has_row = false;
    int64_t count = 0;
    LbdbError error = no_command_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,created_at,completed_at,scope_type,source_unit_id,scope_label,state,"
            "coverage_policy,delivery,template_id,base_question_count FROM quiz_sessions "
            "WHERE state IN('planned','in_progress','paused') ORDER BY id",
            &sessions);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "report.active");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "sessions"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(sessions, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const int64_t quiz_id = lbdb_statement_column_int64(sessions, 0);
        LbdbStatement *current = NULL;
        bool current_row = false;
        if (!lbdb_json_begin_object(app->output) || !lbdb_json_key(app->output, "session")) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build active report");
        }
        if (error == LBDB_OK) {
            error = lbdb_render_session_summary(app, database, app->output, sessions);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(database,
                                           "SELECT id FROM quiz_questions WHERE quiz_id=?1 "
                                           "AND state='asked' ORDER BY position LIMIT 1",
                                           &current);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(current, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = query_error(app, database, lbdb_statement_step(current, &current_row),
                                "Cannot load active question");
        }
        if (error == LBDB_OK && !lbdb_json_key(app->output, "current_question")) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build active report");
        }
        if (error == LBDB_OK) {
            error = current_row
                        ? lbdb_render_public_question(app, database, app->output,
                                                      lbdb_statement_column_int64(current, 0))
                        : (lbdb_json_null(app->output)
                               ? LBDB_OK
                               : lbdb_app_fail(app, LBDB_ERROR_MEMORY,
                                               "Unable to build active report"));
        }
        if (error == LBDB_OK && !lbdb_json_end_object(app->output)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize active report");
        }
        lbdb_statement_destroy(current);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot report active sessions");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(sessions);
    lbdb_database_close(database);
    return error;
}

static LbdbError report_quiz_id(LbdbCommand *command, LbdbArgs *args, int64_t *quiz_id) {
    LbdbStringVector values = {0};
    LbdbError error = lbdb_args_positionals(args, &values);
    if (error == LBDB_OK && values.count != 1U) {
        error = lbdb_app_fail(command->app, LBDB_ERROR_USAGE,
                              "report quiz requires exactly one quiz ID");
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(command->app, "quiz ID", values.items[0], quiz_id);
    }
    lbdb_string_vector_destroy(&values);
    return error;
}

LbdbError lbdb_command_report_quiz(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    int64_t quiz_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = report_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        bool session_row = false;
        error = lbdb_statement_prepare(
            database,
            "SELECT id,created_at,completed_at,scope_type,source_unit_id,scope_label,state,"
            "coverage_policy,delivery,template_id,base_question_count FROM quiz_sessions WHERE "
            "id=?1",
            &session);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(session, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = query_error(app, database, lbdb_statement_step(session, &session_row),
                                "Cannot load quiz report");
        }
        if (error == LBDB_OK && !session_row) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz: %lld", (long long)quiz_id);
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "report.quiz");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "session"));
        error = lbdb_render_session_summary(app, database, app->output, session);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,position,section_title,concept,importance,source_pages,source_line_start,"
            "source_line_end,justification,concept_id FROM quiz_objectives WHERE quiz_id=?1 "
            "ORDER BY position",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "objectives"));
            error = lbdb_output_statement_rows(app, statement);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT id FROM quiz_questions WHERE quiz_id=?1 "
                                       "ORDER BY position",
                                       &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
            LBDB_JSON(app, lbdb_json_begin_array(app->output));
        }
        while (error == LBDB_OK) {
            error = lbdb_statement_step(statement, &has_row);
            if (error != LBDB_OK || !has_row) {
                break;
            }
            const int64_t question_id = lbdb_statement_column_int64(statement, 0);
            LbdbStatement *responses = NULL;
            if (!lbdb_json_begin_object(app->output) || !lbdb_json_key(app->output, "question")) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz report");
            }
            if (error == LBDB_OK) {
                error = lbdb_render_public_question(app, database, app->output, question_id);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_prepare(
                    database,
                    "SELECT id,attempt_number,answered_at,answer,assessment,feedback "
                    "FROM quiz_responses WHERE question_id=?1 ORDER BY attempt_number",
                    &responses);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_int64(responses, 1, question_id);
            }
            if (error == LBDB_OK) {
                LBDB_JSON(app, lbdb_json_key(app->output, "responses"));
                error = lbdb_output_statement_rows(app, responses);
            }
            if (error == LBDB_OK && !lbdb_json_end_object(app->output)) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize quiz report");
            }
            lbdb_statement_destroy(responses);
        }
        if (error == LBDB_ERROR_SQLITE) {
            error = lbdb_app_database_error(app, database, error, "Cannot list quiz questions");
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_end_array(app->output));
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,question_id,response_id,event_type,reason,payload_json,created_at "
            "FROM quiz_events WHERE quiz_id=?1 ORDER BY id",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "events"));
            LBDB_JSON(app, lbdb_json_begin_array(app->output));
        }
        while (error == LBDB_OK) {
            error = lbdb_statement_step(statement, &has_row);
            if (error != LBDB_OK || !has_row) {
                break;
            }
            if (!lbdb_json_begin_object(app->output) || !lbdb_json_key(app->output, "id") ||
                !lbdb_json_int(app->output, lbdb_statement_column_int64(statement, 0)) ||
                !lbdb_json_key(app->output, "question_id")) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz event");
            }
            for (int column = 1; error == LBDB_OK && column <= 2; ++column) {
                if (lbdb_statement_column_is_null(statement, column)) {
                    LBDB_JSON(app, lbdb_json_null(app->output));
                } else {
                    LBDB_JSON(app, lbdb_json_int(app->output,
                                                 lbdb_statement_column_int64(statement, column)));
                }
                if (column == 1) {
                    LBDB_JSON(app, lbdb_json_key(app->output, "response_id"));
                }
            }
            if (error == LBDB_OK &&
                (!lbdb_json_key(app->output, "event_type") ||
                 !lbdb_json_string(app->output, lbdb_statement_column_text(statement, 3)) ||
                 !lbdb_json_key(app->output, "reason") ||
                 !lbdb_json_string_or_null(app->output, lbdb_statement_column_text(statement, 4)) ||
                 !lbdb_json_key(app->output, "payload") ||
                 !lbdb_json_raw(app->output, lbdb_statement_column_text(statement, 5)) ||
                 !lbdb_json_key(app->output, "created_at") ||
                 !lbdb_json_string(app->output, lbdb_statement_column_text(statement, 6)) ||
                 !lbdb_json_end_object(app->output))) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz event");
            }
        }
        if (error == LBDB_ERROR_SQLITE) {
            error = lbdb_app_database_error(app, database, error, "Cannot list quiz events");
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_end_array(app->output));
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_report_coverage(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    LbdbError error = no_command_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "report.coverage");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT * FROM source_coverage "
                                       "ORDER BY corpus_slug,unit_id",
                                       &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "units"));
            error = lbdb_output_statement_rows(app, statement);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT u.id AS unit_id,u.title AS unit_title,s.id AS section_id,"
            "s.title AS section_title FROM source_sections s JOIN source_units u ON u.id=s.unit_id "
            "LEFT JOIN concept_sources cs ON cs.section_id=s.id WHERE s.is_summary=0 "
            "GROUP BY s.id HAVING count(cs.concept_id)=0 ORDER BY "
            "u.corpus_slug,u.position,s.position",
            &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "uncovered_sections"));
            error = lbdb_output_statement_rows(app, statement);
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_report_mastery(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    LbdbError error = no_command_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "report.mastery");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "WITH ranked AS(SELECT "
            "id,created_at,topic,state,evidence,next_step,source_type,quiz_id,"
            "quiz_response_id,row_number() OVER(PARTITION BY topic COLLATE NOCASE "
            "ORDER BY created_at DESC,id DESC) rank FROM learning_records) "
            "SELECT id,created_at,topic,state,evidence,next_step,source_type,quiz_id,"
            "quiz_response_id FROM ranked WHERE rank=1 ORDER BY topic COLLATE NOCASE",
            &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "topics"));
            LBDB_JSON(app, lbdb_json_begin_array(app->output));
            bool has_row = false;
            while (error == LBDB_OK) {
                error = lbdb_statement_step(statement, &has_row);
                if (error != LBDB_OK || !has_row) {
                    break;
                }
                error = write_learning_record(app, app->output, statement);
            }
            if (error == LBDB_ERROR_SQLITE) {
                error =
                    lbdb_app_database_error(app, database, error, "Cannot report mastery topics");
            }
            if (error == LBDB_OK) {
                LBDB_JSON(app, lbdb_json_end_array(app->output));
            }
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "WITH ranked AS(SELECT state,row_number() OVER(PARTITION BY topic COLLATE NOCASE "
            "ORDER BY created_at DESC,id DESC) rank FROM learning_records) "
            "SELECT state,count(*) AS count FROM ranked WHERE rank=1 GROUP BY state ORDER BY state",
            &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "latest_learning_state_counts"));
            error = lbdb_output_statement_rows(app, statement);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "WITH ranked AS(SELECT assessment,row_number() OVER(PARTITION BY question_id "
            "ORDER BY attempt_number DESC,id DESC) rank FROM quiz_responses) "
            "SELECT assessment,count(*) AS count FROM ranked WHERE rank=1 "
            "GROUP BY assessment ORDER BY assessment",
            &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "latest_assessment_counts"));
            error = lbdb_output_statement_rows(app, statement);
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    return error;
}

static LbdbError append_drift_rows(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                   const char *type, const char *sql, int64_t *count) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(database, sql, &statement);
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "type") ||
            !lbdb_json_string(writer, type)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build drift report");
            break;
        }
        const int columns = lbdb_statement_column_count(statement);
        for (int column = 0; error == LBDB_OK && column < columns; ++column) {
            if (!lbdb_json_key(writer, lbdb_statement_column_name(statement, column))) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build drift report");
            } else if (lbdb_statement_column_is_null(statement, column)) {
                LBDB_JSON(app, lbdb_json_null(writer));
            } else if (lbdb_statement_column_type(statement, column) == LBDB_COLUMN_INTEGER) {
                LBDB_JSON(app,
                          lbdb_json_int(writer, lbdb_statement_column_int64(statement, column)));
            } else {
                LBDB_JSON(app,
                          lbdb_json_string(writer, lbdb_statement_column_text(statement, column)));
            }
        }
        if (error == LBDB_OK && !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize drift report");
        }
        if (error == LBDB_OK) {
            *count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot inspect state drift");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static const char *state_word(const char *line) {
    static const char *const states[] = {"planned", "in_progress", "paused", "completed",
                                         "abandoned"};
    for (size_t index = 0; index < sizeof(states) / sizeof(states[0]); ++index) {
        if (strstr(line, states[index]) != NULL) {
            return states[index];
        }
    }
    return NULL;
}

static LbdbError append_markdown_drift(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                       int64_t *count, char **learning_path_output) {
    char *path = lbdb_string_format("%s/.book-learning/LEARNING.md", app->root);
    char *contents = NULL;
    size_t size = 0U;
    LbdbError error = LBDB_OK;
    if (path == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate learning-state path");
    }
    *learning_path_output = lbdb_string_duplicate(path);
    if (*learning_path_output == NULL) {
        free(path);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate learning-state path");
    }
    if (!lbdb_path_is_file(path)) {
        free(path);
        return LBDB_OK;
    }
    error = lbdb_read_file(app, path, &contents, &size);
    free(path);
    if (error != LBDB_OK) {
        return error;
    }
    size_t line_number = 1U;
    char *line = contents;
    for (size_t index = 0; error == LBDB_OK && index <= size; ++index) {
        if (index != size && contents[index] != '\n') {
            continue;
        }
        contents[index] = '\0';
        char *marker = strstr(line, "quiz_id=");
        while (marker != NULL && error == LBDB_OK) {
            char *end = NULL;
            errno = 0;
            const long long parsed = strtoll(marker + 8, &end, 10);
            if (errno == 0 && end != marker + 8 && parsed > 0) {
                LbdbStatement *statement = NULL;
                bool has_row = false;
                error = lbdb_statement_prepare(
                    database, "SELECT state FROM quiz_sessions WHERE id=?1", &statement);
                if (error == LBDB_OK) {
                    error = lbdb_statement_bind_int64(statement, 1, (int64_t)parsed);
                }
                if (error == LBDB_OK) {
                    error = query_error(app, database, lbdb_statement_step(statement, &has_row),
                                        "Cannot compare learning-state quiz");
                }
                const char *expected = state_word(line);
                const char *actual = has_row ? lbdb_statement_column_text(statement, 0) : NULL;
                if (error == LBDB_OK &&
                    (!has_row || (expected != NULL && strcmp(expected, actual) != 0))) {
                    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "type") ||
                        !lbdb_json_string(writer, has_row ? "learning_markdown_state_mismatch"
                                                          : "learning_markdown_unknown_quiz") ||
                        !lbdb_json_key(writer, "quiz_id") ||
                        !lbdb_json_int(writer, (int64_t)parsed) || !lbdb_json_key(writer, "line") ||
                        !lbdb_json_int(writer, (int64_t)line_number)) {
                        error =
                            lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build markdown drift");
                    }
                    if (error == LBDB_OK && has_row &&
                        (!lbdb_json_key(writer, "markdown_state") ||
                         !lbdb_json_string(writer, expected) ||
                         !lbdb_json_key(writer, "database_state") ||
                         !lbdb_json_string(writer, actual))) {
                        error =
                            lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build markdown drift");
                    }
                    if (error == LBDB_OK && !lbdb_json_end_object(writer)) {
                        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY,
                                              "Unable to finalize markdown drift");
                    }
                    if (error == LBDB_OK) {
                        *count += 1;
                    }
                }
                lbdb_statement_destroy(statement);
            }
            marker = strstr(marker + 8, "quiz_id=");
        }
        line = contents + index + 1U;
        line_number += 1U;
    }
    free(contents);
    return error;
}

LbdbError lbdb_command_report_state_drift(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    char *learning_path = NULL;
    int64_t count = 0;
    LbdbError error = no_command_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "report.state-drift");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "drift"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
        error = append_drift_rows(
            app, database, app->output, "multiple_asked_questions",
            "SELECT quiz_id,count(*) AS asked_count FROM quiz_questions WHERE state='asked' "
            "GROUP BY quiz_id HAVING count(*)>1",
            &count);
    }
    if (error == LBDB_OK) {
        error = append_drift_rows(
            app, database, app->output, "answered_without_response",
            "SELECT q.id AS question_id,q.quiz_id FROM quiz_questions q WHERE q.state='answered' "
            "AND NOT EXISTS(SELECT 1 FROM quiz_responses r WHERE r.question_id=q.id)",
            &count);
    }
    if (error == LBDB_OK) {
        error = append_drift_rows(
            app, database, app->output, "completed_with_unresolved_questions",
            "SELECT s.id AS quiz_id,count(q.id) AS unresolved_count FROM quiz_sessions s "
            "JOIN quiz_questions q ON q.quiz_id=s.id AND q.state IN('planned','asked','deferred') "
            "WHERE s.state='completed' GROUP BY s.id",
            &count);
    }
    if (error == LBDB_OK) {
        error = append_drift_rows(
            app, database, app->output, "base_count_mismatch",
            "SELECT s.id AS quiz_id,s.base_question_count,count(q.id) AS snapshot_base_count "
            "FROM quiz_sessions s LEFT JOIN quiz_questions q ON q.quiz_id=s.id AND q.origin='base' "
            "GROUP BY s.id HAVING s.base_question_count<>count(q.id)",
            &count);
    }
    if (error == LBDB_OK) {
        error = append_markdown_drift(app, database, app->output, &count, &learning_path);
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "in_sync"));
        LBDB_JSON(app, lbdb_json_bool(app->output, count == 0));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        LBDB_JSON(app, lbdb_json_key(app->output, "learning_state"));
        LBDB_JSON(app, lbdb_json_string_or_null(app->output, learning_path));
        error = lbdb_output_end(app);
    }
    free(learning_path);
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_stats(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = no_command_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT (SELECT count(*) FROM source_units) AS units,"
            "(SELECT count(*) FROM source_sections) AS sections,"
            "(SELECT count(*) FROM concepts) AS concepts,"
            "(SELECT count(*) FROM question_bank WHERE active=1) AS active_questions,"
            "(SELECT count(*) FROM question_bank WHERE active=0) AS retired_questions,"
            "(SELECT count(*) FROM quiz_templates WHERE active=1) AS active_templates,"
            "(SELECT count(*) FROM quiz_sessions) AS quiz_sessions,"
            "(SELECT count(*) FROM quiz_responses) AS responses,"
            "(SELECT count(*) FROM learning_records) AS learning_records,"
            "(SELECT count(*) FROM quiz_events) AS quiz_events,"
            "(SELECT count(*) FROM db_operations) AS db_operations",
            &statement);
    }
    if (error == LBDB_OK) {
        error = query_error(app, database, lbdb_statement_step(statement, &has_row),
                            "Cannot load statistics");
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "stats");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "counts"));
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        const int columns = lbdb_statement_column_count(statement);
        for (int column = 0; column < columns; ++column) {
            LBDB_JSON(app,
                      lbdb_json_key(app->output, lbdb_statement_column_name(statement, column)));
            LBDB_JSON(app,
                      lbdb_json_int(app->output, lbdb_statement_column_int64(statement, column)));
        }
        LBDB_JSON(app, lbdb_json_end_object(app->output));
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT state,count(*) AS count FROM quiz_sessions "
                                       "GROUP BY state ORDER BY state",
                                       &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "session_states"));
            error = lbdb_output_statement_rows(app, statement);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT state,count(*) AS count FROM quiz_questions "
                                       "GROUP BY state ORDER BY state",
                                       &statement);
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_key(app->output, "question_states"));
            error = lbdb_output_statement_rows(app, statement);
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    return error;
}
