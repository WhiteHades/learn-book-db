#include "internal.h"

#include <errno.h>
#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct LbdbHealth {
    bool json_functions;
    bool tables;
    bool views;
    bool integrity;
    bool foreign_keys;
    bool identity;
    bool migration;
    bool schema_fingerprint;
    int64_t missing_tables;
    int64_t missing_views;
    int64_t foreign_key_failures;
    int64_t schema_version;
    int64_t interface_version;
    int64_t user_version;
    char *interface_name;
    char *migration_checksum;
} LbdbHealth;

static void health_destroy(LbdbHealth *health) {
    free(health->interface_name);
    free(health->migration_checksum);
    *health = (LbdbHealth){0};
}

static LbdbError prepare_or_error(LbdbApp *app, LbdbDatabase *database, const char *sql,
                                  LbdbStatement **statement);
static LbdbError step_or_error(LbdbApp *app, LbdbDatabase *database, LbdbStatement *statement,
                               bool *has_row);

static void schema_checksum(char output[65]) {
    LbdbSha256 context = {0};
    unsigned char digest[32] = {0};
    lbdb_sha256_init(&context);
    lbdb_sha256_update(&context, lbdb_schema_sql(), lbdb_schema_sql_size());
    lbdb_sha256_final(&context, digest);
    lbdb_sha256_hex(digest, output);
}

static LbdbError schema_fingerprint(LbdbApp *app, LbdbDatabase *database, char output[65]) {
    LbdbStatement *statement = NULL;
    LbdbSha256 context = {0};
    unsigned char digest[32] = {0};
    bool has_row = false;
    LbdbError error = prepare_or_error(
        app, database,
        "SELECT type,name,tbl_name,sql FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' "
        "AND sql IS NOT NULL ORDER BY type,name,tbl_name,sql",
        &statement);
    lbdb_sha256_init(&context);
    while (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        for (int column = 0; column < 4; ++column) {
            const char *value = lbdb_statement_column_text(statement, column);
            lbdb_sha256_update(&context, value, strlen(value));
            lbdb_sha256_update(&context, "\0", 1U);
        }
        lbdb_sha256_update(&context, "\xff", 1U);
    }
    lbdb_statement_destroy(statement);
    if (error == LBDB_OK) {
        lbdb_sha256_final(&context, digest);
        lbdb_sha256_hex(digest, output);
    }
    return error;
}

static LbdbError expected_schema_fingerprint(LbdbApp *app, char output[65]) {
    LbdbDatabase *database = NULL;
    LbdbError error = lbdb_database_open(":memory:", LBDB_DATABASE_CREATE, &database);
    if (error == LBDB_OK) {
        error = lbdb_database_exec_static(database, lbdb_schema_sql());
    }
    if (error == LBDB_OK) {
        error = schema_fingerprint(app, database, output);
    } else if (app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot build expected schema");
    }
    lbdb_database_close(database);
    return error;
}

static LbdbError no_arguments(LbdbCommand *command) {
    LbdbArgs args = {0};
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError prepare_or_error(LbdbApp *app, LbdbDatabase *database, const char *sql,
                                  LbdbStatement **statement) {
    LbdbError error = lbdb_statement_prepare(database, sql, statement);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot prepare database check");
}

static LbdbError step_or_error(LbdbApp *app, LbdbDatabase *database, LbdbStatement *statement,
                               bool *has_row) {
    LbdbError error = lbdb_statement_step(statement, has_row);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot execute database check");
}

static LbdbError object_exists(LbdbApp *app, LbdbDatabase *database, const char *type,
                               const char *name, bool *exists) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = prepare_or_error(
        app, database, "SELECT 1 FROM sqlite_schema WHERE type=?1 AND name=?2", &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, type);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, name);
    }
    if (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
    }
    if (error == LBDB_OK) {
        *exists = has_row;
    } else if (app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot inspect schema object");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError metadata_value(LbdbApp *app, LbdbDatabase *database, const char *key,
                                char **value) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error =
        prepare_or_error(app, database, "SELECT value FROM metadata WHERE key=?1", &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, key);
    }
    if (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
    }
    if (error == LBDB_OK && has_row) {
        *value = lbdb_string_duplicate(lbdb_statement_column_text(statement, 0));
        if (*value == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate metadata value");
        }
    }
    lbdb_statement_destroy(statement);
    return error;
}

static bool parse_integer(const char *value, int64_t *result) {
    char *end = NULL;
    long long parsed = 0;
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *result = (int64_t)parsed;
    return true;
}

static LbdbError health_check(LbdbApp *app, LbdbDatabase *database, LbdbHealth *health) {
    size_t table_count = 0U;
    size_t view_count = 0U;
    const char *const *tables = lbdb_required_tables(&table_count);
    const char *const *views = lbdb_required_views(&view_count);
    LbdbStatement *statement = NULL;
    bool has_row = false;
    bool available = false;
    char *schema_text = NULL;
    char *interface_text = NULL;
    char expected_checksum[65] = {0};
    char expected_fingerprint[65] = {0};
    char actual_fingerprint[65] = {0};
    LbdbError error = lbdb_database_check_json(database, &available);

    if (error != LBDB_OK) {
        return lbdb_app_database_error(app, database, error, "Cannot check SQLite JSON functions");
    }
    health->json_functions = available;
    for (size_t index = 0; index < table_count; ++index) {
        bool exists = false;
        error = object_exists(app, database, "table", tables[index], &exists);
        if (error != LBDB_OK) {
            return error;
        }
        if (!exists) {
            health->missing_tables += 1;
        }
    }
    for (size_t index = 0; index < view_count; ++index) {
        bool exists = false;
        error = object_exists(app, database, "view", views[index], &exists);
        if (error != LBDB_OK) {
            return error;
        }
        if (!exists) {
            health->missing_views += 1;
        }
    }
    health->tables = health->missing_tables == 0;
    health->views = health->missing_views == 0;

    error = expected_schema_fingerprint(app, expected_fingerprint);
    if (error == LBDB_OK) {
        error = schema_fingerprint(app, database, actual_fingerprint);
    }
    if (error != LBDB_OK) {
        return error;
    }
    health->schema_fingerprint = strcmp(expected_fingerprint, actual_fingerprint) == 0;

    error = prepare_or_error(app, database, "PRAGMA integrity_check", &statement);
    if (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
    }
    health->integrity =
        error == LBDB_OK && has_row && strcmp(lbdb_statement_column_text(statement, 0), "ok") == 0;
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error != LBDB_OK) {
        return error;
    }

    error = prepare_or_error(app, database, "SELECT count(*) FROM pragma_foreign_key_check",
                             &statement);
    if (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
    }
    if (error == LBDB_OK && has_row) {
        health->foreign_key_failures = lbdb_statement_column_int64(statement, 0);
        health->foreign_keys = health->foreign_key_failures == 0;
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error != LBDB_OK) {
        return error;
    }

    if (health->tables) {
        error = metadata_value(app, database, "schema_version", &schema_text);
        if (error == LBDB_OK) {
            error = metadata_value(app, database, "db_interface_version", &interface_text);
        }
        if (error == LBDB_OK) {
            error = metadata_value(app, database, "db_interface_name", &health->interface_name);
        }
        if (error != LBDB_OK) {
            free(schema_text);
            free(interface_text);
            return error;
        }
        if (!parse_integer(schema_text, &health->schema_version)) {
            health->schema_version = -1;
        }
        if (!parse_integer(interface_text, &health->interface_version)) {
            health->interface_version = -1;
        }
        free(schema_text);
        free(interface_text);

        error = prepare_or_error(app, database, "PRAGMA user_version", &statement);
        if (error == LBDB_OK) {
            error = step_or_error(app, database, statement, &has_row);
        }
        if (error == LBDB_OK && has_row) {
            health->user_version = lbdb_statement_column_int64(statement, 0);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
        if (error != LBDB_OK) {
            return error;
        }
        health->identity = health->interface_name != NULL &&
                           strcmp(health->interface_name, LBDB_INTERFACE_NAME) == 0 &&
                           health->schema_version == LBDB_SCHEMA_VERSION &&
                           health->interface_version == LBDB_INTERFACE_VERSION &&
                           health->user_version == LBDB_SCHEMA_VERSION;

        error = prepare_or_error(
            app, database,
            "SELECT checksum FROM schema_migrations WHERE version=?1 AND name='schema-v1'",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, LBDB_SCHEMA_VERSION);
        }
        if (error == LBDB_OK) {
            error = step_or_error(app, database, statement, &has_row);
        }
        if (error == LBDB_OK && has_row) {
            health->migration_checksum =
                lbdb_string_duplicate(lbdb_statement_column_text(statement, 0));
            if (health->migration_checksum == NULL) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate migration checksum");
            }
        }
        lbdb_statement_destroy(statement);
        if (error != LBDB_OK) {
            return error;
        }
        schema_checksum(expected_checksum);
        health->migration = health->migration_checksum != NULL &&
                            strcmp(health->migration_checksum, expected_checksum) == 0;
    }
    return LBDB_OK;
}

static bool health_is_ok(const LbdbHealth *health) {
    return health->json_functions && health->tables && health->views && health->integrity &&
           health->foreign_keys && health->identity && health->migration &&
           health->schema_fingerprint;
}

static LbdbError write_health(LbdbApp *app, const LbdbHealth *health) {
    LBDB_JSON(app, lbdb_json_key(app->output, "healthy"));
    LBDB_JSON(app, lbdb_json_bool(app->output, health_is_ok(health)));
    LBDB_JSON(app, lbdb_json_key(app->output, "checks"));
    LBDB_JSON(app, lbdb_json_begin_object(app->output));
#define WRITE_CHECK(name, value)                                                                   \
    do {                                                                                           \
        LBDB_JSON(app, lbdb_json_key(app->output, (name)));                                        \
        LBDB_JSON(app, lbdb_json_bool(app->output, (value)));                                      \
    } while (false)
    WRITE_CHECK("json_functions", health->json_functions);
    WRITE_CHECK("required_tables", health->tables);
    WRITE_CHECK("required_views", health->views);
    WRITE_CHECK("integrity", health->integrity);
    WRITE_CHECK("foreign_keys", health->foreign_keys);
    WRITE_CHECK("identity", health->identity);
    WRITE_CHECK("migration_checksum", health->migration);
    WRITE_CHECK("schema_fingerprint", health->schema_fingerprint);
#undef WRITE_CHECK
    LBDB_JSON(app, lbdb_json_end_object(app->output));
    LBDB_JSON(app, lbdb_json_key(app->output, "versions"));
    LBDB_JSON(app, lbdb_json_begin_object(app->output));
    LBDB_JSON(app, lbdb_json_key(app->output, "schema"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->schema_version));
    LBDB_JSON(app, lbdb_json_key(app->output, "user_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->user_version));
    LBDB_JSON(app, lbdb_json_key(app->output, "interface_name"));
    LBDB_JSON(app, lbdb_json_string_or_null(app->output, health->interface_name));
    LBDB_JSON(app, lbdb_json_key(app->output, "interface_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->interface_version));
    LBDB_JSON(app, lbdb_json_end_object(app->output));
    LBDB_JSON(app, lbdb_json_key(app->output, "missing_tables"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->missing_tables));
    LBDB_JSON(app, lbdb_json_key(app->output, "missing_views"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->missing_views));
    LBDB_JSON(app, lbdb_json_key(app->output, "foreign_key_failures"));
    LBDB_JSON(app, lbdb_json_int(app->output, health->foreign_key_failures));
    return LBDB_OK;
}

static LbdbError insert_metadata(LbdbApp *app, LbdbDatabase *database) {
    static const char *const keys[] = {"audit_policy", "db_interface_name", "db_interface_version",
                                       "indexed_source_count", "schema_version"};
    static const char *const values[] = {
        "Every mutation is transactional and recorded in db_operations.",
        LBDB_INTERFACE_NAME,
        "1",
        "0",
        "1",
    };
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_statement_prepare(
        database, "INSERT INTO metadata(key,value) VALUES(?1,?2)", &statement);
    for (size_t index = 0; error == LBDB_OK && index < sizeof(keys) / sizeof(keys[0]); ++index) {
        error = lbdb_statement_bind_text(statement, 1, keys[index]);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, values[index]);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_step(statement, NULL);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_reset(statement);
        }
    }
    lbdb_statement_destroy(statement);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot initialize metadata");
}

LbdbError lbdb_command_db_init(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    char checksum[65] = {0};
    LbdbError error = no_arguments(command);
    if (error != LBDB_OK) {
        return error;
    }
    if (lbdb_path_exists(app->database_path)) {
        return lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Database already exists: %s",
                             app->database_path);
    }
    LBDB_TRY(lbdb_make_parent_directories(app, app->database_path));
    error = lbdb_app_open_database(app, LBDB_DATABASE_CREATE, &database);
    if (error != LBDB_OK) {
        return error;
    }
    error = lbdb_database_begin_immediate(database);
    if (error == LBDB_OK) {
        error = lbdb_database_exec_static(database, lbdb_schema_sql());
    }
    if (error == LBDB_OK) {
        error = insert_metadata(app, database);
    }
    schema_checksum(checksum);
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO schema_migrations(version,name,checksum) VALUES(?1,'schema-v1',?2)",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, LBDB_SCHEMA_VERSION);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, checksum);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, NULL);
    }
    lbdb_statement_destroy(statement);
    if (error == LBDB_OK) {
        error = lbdb_database_exec_static(database, "PRAGMA user_version=1");
    }
    if (error == LBDB_OK) {
        error = lbdb_commit_write(app, database, "db.init", "database", 0,
                                  "{\"schema_version\":1,\"interface_version\":1}");
    }
    if (error != LBDB_OK) {
        if (app->error == LBDB_OK) {
            (void)lbdb_app_database_error(app, database, error, "Cannot initialize database");
        }
        lbdb_database_rollback(database);
        lbdb_database_close(database);
        (void)unlink(app->database_path);
        char *wal = lbdb_string_format("%s-wal", app->database_path);
        char *shm = lbdb_string_format("%s-shm", app->database_path);
        if (wal != NULL) {
            (void)unlink(wal);
        }
        if (shm != NULL) {
            (void)unlink(shm);
        }
        free(wal);
        free(shm);
        return error;
    }
    lbdb_database_close(database);
    LBDB_TRY(lbdb_output_begin(app, "db.init"));
    LBDB_JSON(app, lbdb_json_key(app->output, "database"));
    LBDB_JSON(app, lbdb_json_string(app->output, app->database_path));
    LBDB_JSON(app, lbdb_json_key(app->output, "schema_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, LBDB_SCHEMA_VERSION));
    LBDB_JSON(app, lbdb_json_key(app->output, "interface_name"));
    LBDB_JSON(app, lbdb_json_string(app->output, LBDB_INTERFACE_NAME));
    LBDB_JSON(app, lbdb_json_key(app->output, "interface_version"));
    LBDB_JSON(app, lbdb_json_int(app->output, LBDB_INTERFACE_VERSION));
    return lbdb_output_end(app);
}

LbdbError lbdb_command_db_doctor(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbHealth health = {0};
    LbdbError error = no_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = health_check(app, database, &health);
    }
    if (error == LBDB_OK && !health_is_ok(&health)) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details != NULL && lbdb_json_begin_object(details) &&
            lbdb_json_key(details, "missing_tables") &&
            lbdb_json_int(details, health.missing_tables) &&
            lbdb_json_key(details, "missing_views") &&
            lbdb_json_int(details, health.missing_views) &&
            lbdb_json_key(details, "foreign_key_failures") &&
            lbdb_json_int(details, health.foreign_key_failures) &&
            lbdb_json_key(details, "schema_fingerprint") &&
            lbdb_json_bool(details, health.schema_fingerprint) && lbdb_json_end_object(details)) {
            error = lbdb_app_fail_details(app, LBDB_ERROR_VALIDATION, lbdb_json_data(details),
                                          "Database health checks failed");
        } else {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Database health checks failed");
        }
        lbdb_json_writer_destroy(details);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "db.doctor");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "database"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->database_path));
        error = write_health(app, &health);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    health_destroy(&health);
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_db_status(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    LbdbHealth health = {0};
    bool has_row = false;
    LbdbError error = no_arguments(command);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = health_check(app, database, &health);
    }
    if (error == LBDB_OK && !health_is_ok(&health)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Database is not a healthy schema-v1 database");
    }
    if (error == LBDB_OK) {
        error = prepare_or_error(
            app, database,
            "SELECT (SELECT count(*) FROM source_units) AS source_units,"
            "(SELECT count(*) FROM concepts) AS concepts,"
            "(SELECT count(*) FROM question_bank) AS bank_questions,"
            "(SELECT count(*) FROM quiz_templates) AS templates,"
            "(SELECT count(*) FROM quiz_sessions) AS quiz_sessions,"
            "(SELECT count(*) FROM quiz_responses) AS responses,"
            "(SELECT count(*) FROM learning_records) AS learning_records,"
            "(SELECT count(*) FROM db_operations) AS operations,"
            "(SELECT count(*) FROM quiz_sessions WHERE state IN('planned','in_progress','paused')) "
            "AS active_sessions",
            &statement);
    }
    if (error == LBDB_OK) {
        error = step_or_error(app, database, statement, &has_row);
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Database count query returned no row");
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "db.status");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "database"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->database_path));
        LBDB_JSON(app, lbdb_json_key(app->output, "versions"));
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "schema"));
        LBDB_JSON(app, lbdb_json_int(app->output, health.schema_version));
        LBDB_JSON(app, lbdb_json_key(app->output, "interface"));
        LBDB_JSON(app, lbdb_json_int(app->output, health.interface_version));
        LBDB_JSON(app, lbdb_json_end_object(app->output));
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
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    health_destroy(&health);
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_db_backup(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbStringVector positionals = {0};
    LbdbDatabase *source = NULL;
    LbdbDatabase *destination = NULL;
    char *output = NULL;
    struct stat status = {0};
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_positionals(&args, &positionals);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && positionals.count != 1U) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "db backup requires exactly one PATH");
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_path(app, positionals.items[0], false, false, &output);
    }
    if (error == LBDB_OK && strcmp(output, app->database_path) == 0) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Backup path must differ from the database");
    }
    if (error == LBDB_OK && lbdb_path_exists(output)) {
        error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Backup destination exists: %s", output);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &source);
    }
    if (error == LBDB_OK) {
        error = lbdb_write_file_exclusive(app, output, "", 0U);
    }
    if (error == LBDB_OK) {
        error = lbdb_database_open(output, LBDB_DATABASE_READ_WRITE, &destination);
        if (error != LBDB_OK) {
            error =
                lbdb_app_database_error(app, destination, error, "Cannot open backup destination");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_database_backup(source, destination);
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, destination, error, "SQLite backup failed");
        }
    }
    lbdb_database_close(destination);
    destination = NULL;
    lbdb_database_close(source);
    source = NULL;
    if (error != LBDB_OK && output != NULL) {
        (void)unlink(output);
    }
    if (error == LBDB_OK && stat(output, &status) != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot inspect backup %s: %s", output,
                              strerror(errno));
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "db.backup");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "database"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->database_path));
        LBDB_JSON(app, lbdb_json_key(app->output, "backup"));
        LBDB_JSON(app, lbdb_json_string(app->output, output));
        LBDB_JSON(app, lbdb_json_key(app->output, "bytes"));
        LBDB_JSON(app, lbdb_json_int(app->output, (int64_t)status.st_size));
        error = lbdb_output_end(app);
    }
    free(output);
    lbdb_string_vector_destroy(&positionals);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError validate_database_path(LbdbApp *app, const char *path, LbdbHealth *health) {
    LbdbDatabase *database = NULL;
    LbdbError error = lbdb_database_open(path, LBDB_DATABASE_READ_ONLY, &database);
    if (error != LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot open restore source");
    }
    if (error == LBDB_OK) {
        error = health_check(app, database, health);
    }
    if (error == LBDB_OK && !health_is_ok(health)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Restore source is not a healthy learn-book-db schema-v1 database");
    }
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_db_restore(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbStringVector positionals = {0};
    bool yes = false;
    char *source_path = NULL;
    char *stage_path = NULL;
    char *wal_path = NULL;
    char *shm_path = NULL;
    LbdbDatabase *source = NULL;
    LbdbDatabase *stage = NULL;
    LbdbHealth source_health = {0};
    LbdbHealth stage_health = {0};
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_flag(&args, "--yes", &yes);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_positionals(&args, &positionals);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && !yes) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "db restore requires --yes");
    }
    if (error == LBDB_OK && positionals.count != 1U) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "db restore requires exactly one PATH");
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_path(app, positionals.items[0], true, false, &source_path);
    }
    if (error == LBDB_OK && strcmp(source_path, app->database_path) == 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Restore source must differ from the destination");
    }
    if (error == LBDB_OK) {
        error = validate_database_path(app, source_path, &source_health);
    }
    if (error == LBDB_OK) {
        stage_path = lbdb_string_format("%s.restore.%ld", app->database_path, (long)getpid());
        wal_path = lbdb_string_format("%s-wal", app->database_path);
        shm_path = lbdb_string_format("%s-shm", app->database_path);
        if (stage_path == NULL || wal_path == NULL || shm_path == NULL) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate restore paths");
            error = LBDB_ERROR_MEMORY;
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_write_file_exclusive(app, stage_path, "", 0U);
    }
    if (error == LBDB_OK) {
        error = lbdb_database_open(source_path, LBDB_DATABASE_READ_ONLY, &source);
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, source, error, "Cannot open restore source");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_database_open(stage_path, LBDB_DATABASE_READ_WRITE, &stage);
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, stage, error, "Cannot open staged restore");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_database_backup(source, stage);
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, stage, error, "Cannot stage restore backup");
        }
    }
    lbdb_database_close(source);
    source = NULL;
    lbdb_database_close(stage);
    stage = NULL;
    if (error == LBDB_OK) {
        error = validate_database_path(app, stage_path, &stage_health);
    }
    if (error == LBDB_OK) {
        error = lbdb_database_open(stage_path, LBDB_DATABASE_READ_WRITE, &stage);
        if (error != LBDB_OK) {
            error =
                lbdb_app_database_error(app, stage, error, "Cannot open staged restore for audit");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_database_begin_immediate(stage);
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "source") || !lbdb_json_string(details, source_path) ||
            !lbdb_json_key(details, "target") || !lbdb_json_string(details, app->database_path) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build restore audit record");
        } else {
            error =
                lbdb_commit_write(app, stage, "db.restore", "database", 0, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error == LBDB_OK) {
        error = lbdb_database_exec_static(
            stage, "PRAGMA wal_checkpoint(TRUNCATE);PRAGMA journal_mode=DELETE");
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, stage, error, "Cannot consolidate staged restore");
        }
    }
    lbdb_database_close(stage);
    stage = NULL;
    if (error == LBDB_OK) {
        (void)unlink(wal_path);
        (void)unlink(shm_path);
        if (rename(stage_path, app->database_path) != 0) {
            error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot replace %s: %s", app->database_path,
                                  strerror(errno));
        }
    }
    if (error != LBDB_OK && stage_path != NULL) {
        (void)unlink(stage_path);
        char *stage_wal = lbdb_string_format("%s-wal", stage_path);
        char *stage_shm = lbdb_string_format("%s-shm", stage_path);
        if (stage_wal != NULL) {
            (void)unlink(stage_wal);
        }
        if (stage_shm != NULL) {
            (void)unlink(stage_shm);
        }
        free(stage_wal);
        free(stage_shm);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "db.restore");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "database"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->database_path));
        LBDB_JSON(app, lbdb_json_key(app->output, "restored_from"));
        LBDB_JSON(app, lbdb_json_string(app->output, source_path));
        error = lbdb_output_end(app);
    }
    lbdb_database_close(source);
    lbdb_database_close(stage);
    health_destroy(&source_health);
    health_destroy(&stage_health);
    free(source_path);
    free(stage_path);
    free(wal_path);
    free(shm_path);
    lbdb_string_vector_destroy(&positionals);
    lbdb_args_destroy(&args);
    return error;
}
