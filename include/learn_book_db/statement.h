#ifndef LEARN_BOOK_DB_STATEMENT_H
#define LEARN_BOOK_DB_STATEMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "learn_book_db/database.h"
#include "learn_book_db/error.h"

typedef struct LbdbStatement LbdbStatement;

typedef enum LbdbColumnType {
    LBDB_COLUMN_INTEGER,
    LBDB_COLUMN_FLOAT,
    LBDB_COLUMN_TEXT,
    LBDB_COLUMN_BLOB,
    LBDB_COLUMN_NULL
} LbdbColumnType;

LbdbError lbdb_statement_prepare(LbdbDatabase *database, const char *sql,
                                 LbdbStatement **out_statement);
void lbdb_statement_destroy(LbdbStatement *statement);
LbdbError lbdb_statement_bind_null(LbdbStatement *statement, int index);
LbdbError lbdb_statement_bind_text(LbdbStatement *statement, int index, const char *value);
LbdbError lbdb_statement_bind_blob(LbdbStatement *statement, int index, const void *value,
                                   size_t size);
LbdbError lbdb_statement_bind_int64(LbdbStatement *statement, int index, int64_t value);
LbdbError lbdb_statement_bind_double(LbdbStatement *statement, int index, double value);
LbdbError lbdb_statement_step(LbdbStatement *statement, bool *has_row);
LbdbError lbdb_statement_reset(LbdbStatement *statement);
int lbdb_statement_column_count(const LbdbStatement *statement);
const char *lbdb_statement_column_name(const LbdbStatement *statement, int column);
const char *lbdb_statement_column_text(const LbdbStatement *statement, int column);
int64_t lbdb_statement_column_int64(const LbdbStatement *statement, int column);
double lbdb_statement_column_double(const LbdbStatement *statement, int column);
bool lbdb_statement_column_is_null(const LbdbStatement *statement, int column);
LbdbColumnType lbdb_statement_column_type(const LbdbStatement *statement, int column);
int64_t lbdb_statement_last_insert_id(const LbdbStatement *statement);
int lbdb_statement_changes(const LbdbStatement *statement);

#endif
