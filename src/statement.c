#include "internal.h"

#include <sqlite3.h>

#include <limits.h>
#include <stdlib.h>

struct LbdbStatement {
    LbdbDatabase *database;
    sqlite3_stmt *handle;
};

LbdbError lbdb_statement_prepare(LbdbDatabase *database, const char *sql,
                                 LbdbStatement **out_statement) {
    LbdbStatement *statement = NULL;
    sqlite3 *handle = lbdb_database_native_handle(database);
    if (database == NULL || sql == NULL || out_statement == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    *out_statement = NULL;
    statement = calloc(1U, sizeof(*statement));
    if (statement == NULL) {
        return LBDB_ERROR_MEMORY;
    }
    statement->database = database;
    if (sqlite3_prepare_v2(handle, sql, -1, &statement->handle, NULL) != SQLITE_OK) {
        *out_statement = statement;
        return LBDB_ERROR_SQLITE;
    }
    *out_statement = statement;
    return LBDB_OK;
}

void lbdb_statement_destroy(LbdbStatement *statement) {
    if (statement == NULL) {
        return;
    }
    if (statement->handle != NULL) {
        sqlite3_finalize(statement->handle);
    }
    free(statement);
}

LbdbError lbdb_statement_bind_null(LbdbStatement *statement, int index) {
    return sqlite3_bind_null(statement->handle, index) == SQLITE_OK ? LBDB_OK : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_bind_text(LbdbStatement *statement, int index, const char *value) {
    if (value == NULL) {
        return lbdb_statement_bind_null(statement, index);
    }
    return sqlite3_bind_text(statement->handle, index, value, -1, SQLITE_TRANSIENT) == SQLITE_OK
               ? LBDB_OK
               : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_bind_blob(LbdbStatement *statement, int index, const void *value,
                                   size_t size) {
    if (size > (size_t)INT_MAX) {
        return LBDB_ERROR_VALIDATION;
    }
    return sqlite3_bind_blob(statement->handle, index, value, (int)size, SQLITE_TRANSIENT) ==
                   SQLITE_OK
               ? LBDB_OK
               : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_bind_int64(LbdbStatement *statement, int index, int64_t value) {
    return sqlite3_bind_int64(statement->handle, index, (sqlite3_int64)value) == SQLITE_OK
               ? LBDB_OK
               : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_bind_double(LbdbStatement *statement, int index, double value) {
    return sqlite3_bind_double(statement->handle, index, value) == SQLITE_OK ? LBDB_OK
                                                                             : LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_step(LbdbStatement *statement, bool *has_row) {
    const int result = sqlite3_step(statement->handle);
    if (result == SQLITE_ROW) {
        if (has_row != NULL) {
            *has_row = true;
        }
        return LBDB_OK;
    }
    if (result == SQLITE_DONE) {
        if (has_row != NULL) {
            *has_row = false;
        }
        return LBDB_OK;
    }
    return LBDB_ERROR_SQLITE;
}

LbdbError lbdb_statement_reset(LbdbStatement *statement) {
    const int reset_result = sqlite3_reset(statement->handle);
    const int clear_result = sqlite3_clear_bindings(statement->handle);
    return reset_result == SQLITE_OK && clear_result == SQLITE_OK ? LBDB_OK : LBDB_ERROR_SQLITE;
}

int lbdb_statement_column_count(const LbdbStatement *statement) {
    return sqlite3_column_count(statement->handle);
}

const char *lbdb_statement_column_name(const LbdbStatement *statement, int column) {
    return sqlite3_column_name(statement->handle, column);
}

const char *lbdb_statement_column_text(const LbdbStatement *statement, int column) {
    return (const char *)sqlite3_column_text(statement->handle, column);
}

int64_t lbdb_statement_column_int64(const LbdbStatement *statement, int column) {
    return (int64_t)sqlite3_column_int64(statement->handle, column);
}

double lbdb_statement_column_double(const LbdbStatement *statement, int column) {
    return sqlite3_column_double(statement->handle, column);
}

bool lbdb_statement_column_is_null(const LbdbStatement *statement, int column) {
    return sqlite3_column_type(statement->handle, column) == SQLITE_NULL;
}

LbdbColumnType lbdb_statement_column_type(const LbdbStatement *statement, int column) {
    switch (sqlite3_column_type(statement->handle, column)) {
    case SQLITE_INTEGER:
        return LBDB_COLUMN_INTEGER;
    case SQLITE_FLOAT:
        return LBDB_COLUMN_FLOAT;
    case SQLITE_TEXT:
        return LBDB_COLUMN_TEXT;
    case SQLITE_BLOB:
        return LBDB_COLUMN_BLOB;
    case SQLITE_NULL:
    default:
        return LBDB_COLUMN_NULL;
    }
}

int64_t lbdb_statement_last_insert_id(const LbdbStatement *statement) {
    return (int64_t)sqlite3_last_insert_rowid(lbdb_database_native_handle(statement->database));
}

int lbdb_statement_changes(const LbdbStatement *statement) {
    return sqlite3_changes(lbdb_database_native_handle(statement->database));
}
