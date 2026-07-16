#include "internal.h"

#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>

struct LbdbDatabase {
    sqlite3 *handle;
    bool read_only;
};

static char *path_to_uri(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t encoded_size = strlen("file:");
    char *uri = NULL;
    char *cursor = NULL;

    for (const unsigned char *input = (const unsigned char *)path; *input != 0U; ++input) {
        const bool safe = (*input >= 'a' && *input <= 'z') || (*input >= 'A' && *input <= 'Z') ||
                          (*input >= '0' && *input <= '9') || *input == '/' || *input == '-' ||
                          *input == '_' || *input == '.' || *input == '~';
        const size_t increment = safe ? 1U : 3U;
        if (encoded_size > SIZE_MAX - increment) {
            return NULL;
        }
        encoded_size += increment;
    }
    if (encoded_size > SIZE_MAX - strlen("?mode=ro") - 1U) {
        return NULL;
    }
    uri = malloc(encoded_size + strlen("?mode=ro") + 1U);
    if (uri == NULL) {
        return NULL;
    }
    cursor = uri;
    memcpy(cursor, "file:", strlen("file:"));
    cursor += strlen("file:");
    for (const unsigned char *input = (const unsigned char *)path; *input != 0U; ++input) {
        const bool safe = (*input >= 'a' && *input <= 'z') || (*input >= 'A' && *input <= 'Z') ||
                          (*input >= '0' && *input <= '9') || *input == '/' || *input == '-' ||
                          *input == '_' || *input == '.' || *input == '~';
        if (safe) {
            *cursor++ = (char)*input;
        } else {
            *cursor++ = '%';
            *cursor++ = hex[*input >> 4U];
            *cursor++ = hex[*input & 0x0fU];
        }
    }
    memcpy(cursor, "?mode=ro", strlen("?mode=ro") + 1U);
    return uri;
}

LbdbError lbdb_database_open(const char *path, LbdbDatabaseMode mode, LbdbDatabase **out_database) {
    LbdbDatabase *database = NULL;
    char *uri = NULL;
    const char *target = path;
    int flags = 0;
    int result = SQLITE_OK;

    if (path == NULL || out_database == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    *out_database = NULL;
    database = calloc(1U, sizeof(*database));
    if (database == NULL) {
        return LBDB_ERROR_MEMORY;
    }
    if (mode == LBDB_DATABASE_READ_ONLY) {
        uri = path_to_uri(path);
        if (uri == NULL) {
            lbdb_database_close(database);
            return LBDB_ERROR_MEMORY;
        }
        target = uri;
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
        database->read_only = true;
    } else {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
        if (mode == LBDB_DATABASE_CREATE) {
            flags |= SQLITE_OPEN_CREATE;
        }
    }
    result = sqlite3_open_v2(target, &database->handle, flags, NULL);
    free(uri);
    if (result != SQLITE_OK) {
        *out_database = database;
        return LBDB_ERROR_SQLITE;
    }
    sqlite3_extended_result_codes(database->handle, 1);
    if (sqlite3_busy_timeout(database->handle, 5000) != SQLITE_OK ||
        sqlite3_exec(database->handle, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != SQLITE_OK) {
        *out_database = database;
        return LBDB_ERROR_SQLITE;
    }
    if (database->read_only) {
        if (sqlite3_exec(database->handle, "PRAGMA query_only=ON", NULL, NULL, NULL) != SQLITE_OK) {
            *out_database = database;
            return LBDB_ERROR_SQLITE;
        }
    } else if (sqlite3_exec(database->handle, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) !=
                   SQLITE_OK ||
               sqlite3_exec(database->handle, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL) !=
                   SQLITE_OK) {
        *out_database = database;
        return LBDB_ERROR_SQLITE;
    }
    *out_database = database;
    return LBDB_OK;
}

void lbdb_database_close(LbdbDatabase *database) {
    if (database == NULL) {
        return;
    }
    if (database->handle != NULL) {
        sqlite3_close_v2(database->handle);
    }
    free(database);
}

LbdbError lbdb_database_exec_static(LbdbDatabase *database, const char *sql) {
    if (database == NULL || database->handle == NULL || sql == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    return sqlite3_exec(database->handle, sql, NULL, NULL, NULL) == SQLITE_OK ? LBDB_OK
                                                                              : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_database_begin_immediate(LbdbDatabase *database) {
    return lbdb_database_exec_static(database, "BEGIN IMMEDIATE");
}

LbdbError lbdb_database_commit(LbdbDatabase *database) {
    return lbdb_database_exec_static(database, "COMMIT");
}

void lbdb_database_rollback(LbdbDatabase *database) {
    if (database != NULL && database->handle != NULL) {
        (void)sqlite3_exec(database->handle, "ROLLBACK", NULL, NULL, NULL);
    }
}

LbdbError lbdb_database_check_json(LbdbDatabase *database, bool *available) {
    sqlite3_stmt *statement = NULL;
    int result = SQLITE_OK;
    if (database == NULL || available == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    *available = false;
    result = sqlite3_prepare_v2(
        database->handle,
        "SELECT json_valid('{\"check\":true}')=1 AND "
        "json_type(json('{\"check\":true}'))='object' AND json_extract('{\"n\":1}','$.n')=1",
        -1, &statement, NULL);
    if (result != SQLITE_OK) {
        return LBDB_ERROR_SQLITE;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        *available = sqlite3_column_int(statement, 0) == 1;
        result = SQLITE_OK;
    }
    sqlite3_finalize(statement);
    return result == SQLITE_OK ? LBDB_OK : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_database_backup(LbdbDatabase *source, LbdbDatabase *destination) {
    sqlite3_backup *backup = NULL;
    int result = SQLITE_OK;
    if (source == NULL || destination == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    backup = sqlite3_backup_init(destination->handle, "main", source->handle, "main");
    if (backup == NULL) {
        return LBDB_ERROR_SQLITE;
    }
    do {
        result = sqlite3_backup_step(backup, 128);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
            sqlite3_sleep(10);
        }
    } while (result == SQLITE_OK || result == SQLITE_BUSY || result == SQLITE_LOCKED);
    const int finish_result = sqlite3_backup_finish(backup);
    return result == SQLITE_DONE && finish_result == SQLITE_OK ? LBDB_OK : LBDB_ERROR_SQLITE;
}

const char *lbdb_database_message(const LbdbDatabase *database) {
    if (database == NULL || database->handle == NULL) {
        return "SQLite database is unavailable";
    }
    return sqlite3_errmsg(database->handle);
}

void *lbdb_database_native_handle(LbdbDatabase *database) {
    return database == NULL ? NULL : database->handle;
}
