#include "internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_error(LbdbApp *app) {
    free(app->error_message);
    free(app->error_details);
    app->error_message = NULL;
    app->error_details = NULL;
    app->error = LBDB_OK;
}

static LbdbError set_error_variadic(LbdbApp *app, LbdbError error, const char *details_json,
                                    const char *format, va_list *measurement, va_list *rendering)
    LBDB_PRINTF_LIKE(4, 0);

static LbdbError set_error_variadic(LbdbApp *app, LbdbError error, const char *details_json,
                                    const char *format, va_list *measurement, va_list *rendering) {
    int needed = 0;
    char *message = NULL;
    if (app == NULL) {
        return error;
    }
    needed = vsnprintf(NULL, 0U, format, *measurement);
    if (needed >= 0) {
        message = malloc((size_t)needed + 1U);
        if (message != NULL) {
            (void)vsnprintf(message, (size_t)needed + 1U, format, *rendering);
        }
    }
    clear_error(app);
    app->error = error;
    app->error_message =
        message != NULL ? message : lbdb_string_duplicate("Unable to format error");
    app->error_details = lbdb_string_duplicate(details_json != NULL ? details_json : "null");
    return error;
}

LbdbError lbdb_app_fail(LbdbApp *app, LbdbError error, const char *format, ...) {
    va_list arguments;
    va_list measurement;
    LbdbError result = error;
    va_start(arguments, format);
    va_copy(measurement, arguments);
    result = set_error_variadic(app, error, NULL, format, &measurement, &arguments);
    va_end(measurement);
    va_end(arguments);
    return result;
}

LbdbError lbdb_app_fail_details(LbdbApp *app, LbdbError error, const char *details_json,
                                const char *format, ...) {
    va_list arguments;
    va_list measurement;
    LbdbError result = error;
    va_start(arguments, format);
    va_copy(measurement, arguments);
    result = set_error_variadic(app, error, details_json, format, &measurement, &arguments);
    va_end(measurement);
    va_end(arguments);
    return result;
}

LbdbError lbdb_app_database_error(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                                  const char *operation) {
    return lbdb_app_fail(app, error, "%s: %s", operation, lbdb_database_message(database));
}

static char *current_directory(void) {
    size_t size = 256U;
    while (size <= (size_t)1U << 20U) {
        char *buffer = malloc(size);
        if (buffer == NULL) {
            return NULL;
        }
        if (getcwd(buffer, size) != NULL) {
            return buffer;
        }
        free(buffer);
        if (errno != ERANGE) {
            return NULL;
        }
        size *= 2U;
    }
    return NULL;
}

static LbdbError parse_global_options(LbdbApp *app) {
    const char *root_option = NULL;
    const char *database_option = NULL;
    const char *manifest_option = NULL;
    char *cwd = current_directory();
    char *root_input = NULL;
    char *resolved_root = NULL;
    char *resolved_database = NULL;
    char *resolved_manifest = NULL;
    int index = 1;

    if (cwd == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot determine current directory");
    }
    while (index < app->argc) {
        const char *argument = app->argv[index];
        if (strcmp(argument, "--pretty") == 0) {
            app->pretty = true;
            index += 1;
        } else if (strcmp(argument, "--help") == 0) {
            app->help_requested = true;
            index += 1;
        } else if (strcmp(argument, "--version") == 0) {
            app->version_requested = true;
            index += 1;
        } else if (strcmp(argument, "--root") == 0 || strcmp(argument, "--db") == 0 ||
                   strcmp(argument, "--manifest") == 0) {
            const char **destination = strcmp(argument, "--root") == 0 ? &root_option
                                       : strcmp(argument, "--db") == 0 ? &database_option
                                                                       : &manifest_option;
            if (*destination != NULL) {
                free(cwd);
                return lbdb_app_fail(app, LBDB_ERROR_USAGE, "Duplicate global option: %s",
                                     argument);
            }
            if (index + 1 >= app->argc) {
                free(cwd);
                return lbdb_app_fail(app, LBDB_ERROR_USAGE, "Missing value for %s", argument);
            }
            *destination = app->argv[index + 1];
            index += 2;
        } else if (argument[0] == '-') {
            free(cwd);
            return lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown global option: %s", argument);
        } else {
            break;
        }
    }
    app->command_start = index;
    root_input = root_option == NULL || root_option[0] == '/'
                     ? lbdb_string_duplicate(root_option != NULL ? root_option : cwd)
                     : lbdb_string_format("%s/%s", cwd, root_option);
    free(cwd);
    if (root_input == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate root path");
    }
    app->root = lbdb_string_duplicate("/");
    if (app->root == NULL) {
        free(root_input);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate root path");
    }
    LbdbError error = lbdb_resolve_path(app, root_input, true, false, &resolved_root);
    free(root_input);
    if (error != LBDB_OK) {
        return error;
    }
    free(app->root);
    app->root = resolved_root;
    LBDB_TRY(lbdb_resolve_path(app, database_option != NULL ? database_option : LBDB_DEFAULT_DB,
                               false, false, &resolved_database));
    LBDB_TRY(lbdb_resolve_path(app,
                               manifest_option != NULL ? manifest_option : LBDB_DEFAULT_MANIFEST,
                               false, false, &resolved_manifest));
    app->database_path = resolved_database;
    app->manifest_path = resolved_manifest;
    return LBDB_OK;
}

LbdbApp *lbdb_app_create(int argc, char *const argv[]) {
    LbdbApp *app = calloc(1U, sizeof(*app));
    if (app == NULL || argc < 0) {
        free(app);
        return NULL;
    }
    app->argc = argc;
    app->argv = calloc((size_t)argc + 1U, sizeof(*app->argv));
    if (app->argv == NULL) {
        free(app);
        return NULL;
    }
    for (int index = 0; index < argc; ++index) {
        app->argv[index] = lbdb_string_duplicate(argv[index]);
        if (app->argv[index] == NULL) {
            lbdb_app_destroy(app);
            return NULL;
        }
    }
    app->output = lbdb_json_writer_create(false);
    if (app->output == NULL) {
        lbdb_app_destroy(app);
        return NULL;
    }
    return app;
}

void lbdb_app_destroy(LbdbApp *app) {
    if (app == NULL) {
        return;
    }
    lbdb_command_destroy(app->command);
    for (int index = 0; index < app->argc; ++index) {
        free(app->argv[index]);
    }
    free(app->argv);
    free(app->root);
    free(app->database_path);
    free(app->manifest_path);
    clear_error(app);
    lbdb_json_writer_destroy(app->output);
    free(app);
}

static LbdbError write_version(LbdbApp *app) {
    LBDB_TRY(lbdb_output_begin(app, "version"));
    LBDB_JSON(app, lbdb_json_key(app->output, "name"));
    LBDB_JSON(app, lbdb_json_string(app->output, LBDB_INTERFACE_NAME));
    LBDB_JSON(app, lbdb_json_key(app->output, "version"));
    LBDB_JSON(app, lbdb_json_string(app->output, LBDB_VERSION_STRING));
    LBDB_JSON(app, lbdb_json_key(app->output, "interface_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, LBDB_INTERFACE_VERSION));
    LBDB_JSON(app, lbdb_json_key(app->output, "schema_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, LBDB_SCHEMA_VERSION));
    return lbdb_output_end(app);
}

static bool write_error_output(LbdbApp *app, FILE *stream) {
    LbdbJsonWriter *writer = lbdb_json_writer_create(app->pretty);
    bool ok = writer != NULL;
    ok = ok && lbdb_json_begin_object(writer);
    ok = ok && lbdb_json_key(writer, "ok") && lbdb_json_bool(writer, false);
    ok = ok && lbdb_json_key(writer, "error") && lbdb_json_begin_object(writer);
    ok = ok && lbdb_json_key(writer, "code") &&
         lbdb_json_string(writer, lbdb_error_name(app->error));
    ok =
        ok && lbdb_json_key(writer, "message") &&
        lbdb_json_string(writer, app->error_message != NULL ? app->error_message : "Unknown error");
    ok = ok && lbdb_json_key(writer, "details") &&
         lbdb_json_raw(writer, app->error_details != NULL ? app->error_details : "null");
    ok = ok && lbdb_json_end_object(writer) && lbdb_json_end_object(writer);
    ok = ok && lbdb_json_write(writer, stream);
    lbdb_json_writer_destroy(writer);
    return ok;
}

int lbdb_app_run(LbdbApp *app, FILE *standard_output, FILE *standard_error) {
    LbdbError error = LBDB_OK;
    clear_error(app);
    error = parse_global_options(app);
    if (error == LBDB_OK && app->help_requested) {
        error = lbdb_command_write_help(app, app->argc - app->command_start,
                                        &app->argv[app->command_start]);
    } else if (error == LBDB_OK && app->version_requested) {
        error = write_version(app);
    } else if (error == LBDB_OK) {
        error = lbdb_command_create(app, app->argc - app->command_start,
                                    &app->argv[app->command_start], &app->command);
        if (error == LBDB_OK) {
            error = lbdb_command_execute(app->command);
        }
    }
    if (app->error != LBDB_OK) {
        error = app->error;
    }
    if (error == LBDB_OK) {
        if (!lbdb_json_write(app->output, standard_output)) {
            error = lbdb_app_fail(app, LBDB_ERROR_IO, "Unable to write command output");
        }
    }
    if (error != LBDB_OK) {
        if (app->error == LBDB_OK) {
            (void)lbdb_app_fail(app, error, "Command failed");
        }
        (void)write_error_output(app, standard_error);
    }
    return lbdb_error_exit_code(error);
}

LbdbError lbdb_app_error(const LbdbApp *app) {
    return app == NULL ? LBDB_ERROR_INTERNAL : app->error;
}

const char *lbdb_app_error_message(const LbdbApp *app) {
    return app == NULL || app->error_message == NULL ? "" : app->error_message;
}

LbdbError lbdb_output_begin(LbdbApp *app, const char *command_name) {
    if (!lbdb_json_writer_reset(app->output, app->pretty) || !lbdb_json_begin_object(app->output) ||
        !lbdb_json_key(app->output, "ok") || !lbdb_json_bool(app->output, true) ||
        !lbdb_json_key(app->output, "command") || !lbdb_json_string(app->output, command_name)) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to initialize JSON output");
    }
    return LBDB_OK;
}

LbdbError lbdb_output_end(LbdbApp *app) {
    if (!lbdb_json_end_object(app->output)) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize JSON output");
    }
    return LBDB_OK;
}

LbdbError lbdb_output_statement_rows(LbdbApp *app, LbdbStatement *statement) {
    bool has_row = false;
    LBDB_JSON(app, lbdb_json_begin_array(app->output));
    for (;;) {
        LbdbError error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK) {
            return lbdb_app_fail(app, error, "SQLite query failed while writing output");
        }
        if (!has_row) {
            break;
        }
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        const int columns = lbdb_statement_column_count(statement);
        for (int column = 0; column < columns; ++column) {
            LBDB_JSON(app,
                      lbdb_json_key(app->output, lbdb_statement_column_name(statement, column)));
            switch (lbdb_statement_column_type(statement, column)) {
            case LBDB_COLUMN_INTEGER:
                LBDB_JSON(app, lbdb_json_int(app->output,
                                             lbdb_statement_column_int64(statement, column)));
                break;
            case LBDB_COLUMN_FLOAT:
                LBDB_JSON(app, lbdb_json_double(app->output,
                                                lbdb_statement_column_double(statement, column)));
                break;
            case LBDB_COLUMN_TEXT:
                LBDB_JSON(app, lbdb_json_string(app->output,
                                                lbdb_statement_column_text(statement, column)));
                break;
            case LBDB_COLUMN_BLOB:
                return lbdb_app_fail(app, LBDB_ERROR_INTERNAL,
                                     "Binary database column cannot be emitted as JSON");
            case LBDB_COLUMN_NULL:
                LBDB_JSON(app, lbdb_json_null(app->output));
                break;
            }
        }
        LBDB_JSON(app, lbdb_json_end_object(app->output));
    }
    LBDB_JSON(app, lbdb_json_end_array(app->output));
    return LBDB_OK;
}

LbdbError lbdb_app_open_database(LbdbApp *app, LbdbDatabaseMode mode, LbdbDatabase **out_database) {
    LbdbDatabase *database = NULL;
    bool json_available = false;
    LbdbError error = LBDB_OK;
    if (mode == LBDB_DATABASE_READ_ONLY && !lbdb_path_is_file(app->database_path)) {
        return lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Database does not exist: %s",
                             app->database_path);
    }
    error = lbdb_database_open(app->database_path, mode, &database);
    if (error != LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot open database");
        lbdb_database_close(database);
        return error;
    }
    error = lbdb_database_check_json(database, &json_available);
    if (error != LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot check SQLite JSON functions");
        lbdb_database_close(database);
        return error;
    }
    if (!json_available) {
        lbdb_database_close(database);
        return lbdb_app_fail(app, LBDB_ERROR_UNSUPPORTED,
                             "SQLite runtime does not provide required JSON functions");
    }
    *out_database = database;
    return LBDB_OK;
}

LbdbError lbdb_begin_write(LbdbApp *app, LbdbDatabase **database) {
    LbdbError error = lbdb_app_open_database(app, LBDB_DATABASE_READ_WRITE, database);
    if (error != LBDB_OK) {
        return error;
    }
    error = lbdb_database_begin_immediate(*database);
    if (error != LBDB_OK) {
        error = lbdb_app_database_error(app, *database, error, "Cannot begin write transaction");
        lbdb_database_close(*database);
        *database = NULL;
    }
    return error;
}

LbdbError lbdb_record_operation(LbdbApp *app, LbdbDatabase *database, const char *command,
                                const char *entity_type, int64_t entity_id,
                                const char *details_json) {
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_statement_prepare(
        database,
        "INSERT INTO db_operations(command,entity_type,entity_id,details_json) VALUES(?1,?2,?3,?4)",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, command);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, entity_type);
    }
    if (error == LBDB_OK) {
        error = entity_id > 0 ? lbdb_statement_bind_int64(statement, 3, entity_id)
                              : lbdb_statement_bind_null(statement, 3);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 4, details_json != NULL ? details_json : "{}");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, NULL);
    }
    lbdb_statement_destroy(statement);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot record database operation");
}

LbdbError lbdb_commit_write(LbdbApp *app, LbdbDatabase *database, const char *command,
                            const char *entity_type, int64_t entity_id, const char *details_json) {
    LbdbError error =
        lbdb_record_operation(app, database, command, entity_type, entity_id, details_json);
    if (error == LBDB_OK) {
        error = lbdb_database_commit(database);
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, database, error, "Cannot commit transaction");
        }
    }
    if (error != LBDB_OK) {
        lbdb_database_rollback(database);
    }
    return error;
}

LbdbError lbdb_add_quiz_event(LbdbApp *app, LbdbDatabase *database, int64_t quiz_id,
                              int64_t question_id, int64_t response_id, const char *event_type,
                              const char *reason, const char *payload_json) {
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_statement_prepare(
        database,
        "INSERT INTO quiz_events(quiz_id,question_id,response_id,event_type,reason,payload_json) "
        "VALUES(?1,?2,?3,?4,?5,?6)",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = question_id > 0 ? lbdb_statement_bind_int64(statement, 2, question_id)
                                : lbdb_statement_bind_null(statement, 2);
    }
    if (error == LBDB_OK) {
        error = response_id > 0 ? lbdb_statement_bind_int64(statement, 3, response_id)
                                : lbdb_statement_bind_null(statement, 3);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 4, event_type);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 5, reason);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 6, payload_json != NULL ? payload_json : "{}");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, NULL);
    }
    lbdb_statement_destroy(statement);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot record quiz event");
}

static LbdbError json_scalar_query(LbdbApp *app, LbdbDatabase *database, const char *sql,
                                   const char *json, const char *path, LbdbStatement **statement,
                                   bool *has_row) {
    LbdbError error = lbdb_statement_prepare(database, sql, statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(*statement, 1, json);
    }
    if (error == LBDB_OK && path != NULL) {
        error = lbdb_statement_bind_text(*statement, 2, path);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(*statement, has_row);
    }
    if (error != LBDB_OK) {
        lbdb_statement_destroy(*statement);
        *statement = NULL;
        return lbdb_app_database_error(app, database, error, "Cannot parse JSON");
    }
    return LBDB_OK;
}

LbdbError lbdb_json_document_type(LbdbApp *app, LbdbDatabase *database, const char *json,
                                  const char *expected_type) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LBDB_TRY(json_scalar_query(app, database, "SELECT json_valid(?1),json_type(?1)", json, NULL,
                               &statement, &has_row));
    const bool valid = has_row && lbdb_statement_column_int64(statement, 0) == 1;
    const char *type = valid ? lbdb_statement_column_text(statement, 1) : NULL;
    const bool matches = type != NULL && strcmp(type, expected_type) == 0;
    lbdb_statement_destroy(statement);
    if (!valid) {
        return lbdb_app_fail(app, LBDB_ERROR_JSON, "Input is not valid JSON");
    }
    if (!matches) {
        return lbdb_app_fail(app, LBDB_ERROR_JSON, "JSON root must be %s", expected_type);
    }
    return LBDB_OK;
}

static LbdbError json_get_value(LbdbApp *app, LbdbDatabase *database, const char *json,
                                const char *path, bool required, const char *required_type,
                                char **value) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    char *copy = NULL;
    LBDB_TRY(json_scalar_query(app, database, "SELECT json_type(?1,?2),json_extract(?1,?2)", json,
                               path, &statement, &has_row));
    const char *type = has_row ? lbdb_statement_column_text(statement, 0) : NULL;
    if (type == NULL || strcmp(type, "null") == 0) {
        lbdb_statement_destroy(statement);
        if (required) {
            return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Required JSON field is missing: %s",
                                 path);
        }
        *value = NULL;
        return LBDB_OK;
    }
    if (required_type != NULL && strcmp(type, required_type) != 0) {
        lbdb_statement_destroy(statement);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "JSON field %s must be %s", path,
                             required_type);
    }
    const char *text = lbdb_statement_column_text(statement, 1);
    copy = lbdb_string_duplicate(text != NULL ? text : "");
    lbdb_statement_destroy(statement);
    if (copy == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate JSON value");
    }
    *value = copy;
    return LBDB_OK;
}

LbdbError lbdb_json_get_text(LbdbApp *app, LbdbDatabase *database, const char *json,
                             const char *path, bool required, char **value) {
    return json_get_value(app, database, json, path, required, "text", value);
}

LbdbError lbdb_json_get_raw(LbdbApp *app, LbdbDatabase *database, const char *json,
                            const char *path, bool required, char **value) {
    return json_get_value(app, database, json, path, required, NULL, value);
}

LbdbError lbdb_json_get_int(LbdbApp *app, LbdbDatabase *database, const char *json,
                            const char *path, bool required, int64_t *value, bool *present) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LBDB_TRY(json_scalar_query(app, database, "SELECT json_type(?1,?2),json_extract(?1,?2)", json,
                               path, &statement, &has_row));
    const char *type = has_row ? lbdb_statement_column_text(statement, 0) : NULL;
    if (type == NULL || strcmp(type, "null") == 0) {
        lbdb_statement_destroy(statement);
        if (required) {
            return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Required JSON field is missing: %s",
                                 path);
        }
        *present = false;
        return LBDB_OK;
    }
    if (strcmp(type, "integer") != 0) {
        lbdb_statement_destroy(statement);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "JSON field %s must be an integer", path);
    }
    *value = lbdb_statement_column_int64(statement, 1);
    *present = true;
    lbdb_statement_destroy(statement);
    return LBDB_OK;
}

LbdbError lbdb_json_get_bool(LbdbApp *app, LbdbDatabase *database, const char *json,
                             const char *path, bool required, bool *value, bool *present) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LBDB_TRY(json_scalar_query(app, database, "SELECT json_type(?1,?2),json_extract(?1,?2)", json,
                               path, &statement, &has_row));
    const char *type = has_row ? lbdb_statement_column_text(statement, 0) : NULL;
    if (type == NULL || strcmp(type, "null") == 0) {
        lbdb_statement_destroy(statement);
        if (required) {
            return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Required JSON field is missing: %s",
                                 path);
        }
        *present = false;
        return LBDB_OK;
    }
    if (strcmp(type, "true") != 0 && strcmp(type, "false") != 0) {
        lbdb_statement_destroy(statement);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "JSON field %s must be boolean", path);
    }
    *value = strcmp(type, "true") == 0;
    *present = true;
    lbdb_statement_destroy(statement);
    return LBDB_OK;
}

LbdbError lbdb_json_array_size(LbdbApp *app, LbdbDatabase *database, const char *json,
                               const char *path, size_t *size) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LBDB_TRY(json_scalar_query(app, database, "SELECT json_type(?1,?2),json_array_length(?1,?2)",
                               json, path, &statement, &has_row));
    const char *type = has_row ? lbdb_statement_column_text(statement, 0) : NULL;
    if (type == NULL || strcmp(type, "array") != 0) {
        lbdb_statement_destroy(statement);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "JSON field %s must be an array", path);
    }
    const int64_t count = lbdb_statement_column_int64(statement, 1);
    lbdb_statement_destroy(statement);
    if (count < 0 || (uint64_t)count > (uint64_t)SIZE_MAX) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "JSON array is too large: %s", path);
    }
    *size = (size_t)count;
    return LBDB_OK;
}

LbdbError lbdb_args_init(LbdbArgs *args, LbdbCommand *command) {
    *args = (LbdbArgs){.app = command->app,
                       .argc = command->argc,
                       .argv = command->argv,
                       .used = calloc((size_t)command->argc, sizeof(bool))};
    if (command->argc > 0 && args->used == NULL) {
        return lbdb_app_fail(command->app, LBDB_ERROR_MEMORY, "Unable to parse command options");
    }
    return LBDB_OK;
}

void lbdb_args_destroy(LbdbArgs *args) {
    free(args->used);
    *args = (LbdbArgs){0};
}

LbdbError lbdb_args_option(LbdbArgs *args, const char *name, bool required, const char **value) {
    int found = -1;
    *value = NULL;
    for (int index = 0; index < args->argc; ++index) {
        if (!args->used[index] && strcmp(args->argv[index], name) == 0) {
            if (found >= 0) {
                return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Duplicate option: %s", name);
            }
            found = index;
        }
    }
    if (found < 0) {
        return required
                   ? lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Missing required option: %s", name)
                   : LBDB_OK;
    }
    if (found + 1 >= args->argc || args->used[found + 1]) {
        return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Missing value for %s", name);
    }
    args->used[found] = true;
    args->used[found + 1] = true;
    *value = args->argv[found + 1];
    return LBDB_OK;
}

LbdbError lbdb_args_flag(LbdbArgs *args, const char *name, bool *present) {
    *present = false;
    for (int index = 0; index < args->argc; ++index) {
        if (!args->used[index] && strcmp(args->argv[index], name) == 0) {
            if (*present) {
                return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Duplicate flag: %s", name);
            }
            args->used[index] = true;
            *present = true;
        }
    }
    return LBDB_OK;
}

LbdbError lbdb_args_positionals(LbdbArgs *args, LbdbStringVector *values) {
    for (int index = 0; index < args->argc; ++index) {
        if (args->used[index]) {
            continue;
        }
        if (strncmp(args->argv[index], "--", 2U) == 0) {
            return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Unknown option: %s",
                                 args->argv[index]);
        }
        LbdbError error = lbdb_string_vector_push(values, args->argv[index]);
        if (error != LBDB_OK) {
            return lbdb_app_fail(args->app, error, "Unable to store positional argument");
        }
        args->used[index] = true;
    }
    return LBDB_OK;
}

LbdbError lbdb_args_finish(LbdbArgs *args) {
    for (int index = 0; index < args->argc; ++index) {
        if (!args->used[index]) {
            return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Unexpected argument: %s",
                                 args->argv[index]);
        }
    }
    return LBDB_OK;
}

LbdbError lbdb_parse_positive_int(LbdbApp *app, const char *name, const char *value,
                                  int64_t *result) {
    char *end = NULL;
    errno = 0;
    const long long parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0) {
        return lbdb_app_fail(app, LBDB_ERROR_USAGE, "%s must be a positive integer", name);
    }
    *result = (int64_t)parsed;
    return LBDB_OK;
}

bool lbdb_string_in_set(const char *value, const char *const values[], size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(value, values[index]) == 0) {
            return true;
        }
    }
    return false;
}
