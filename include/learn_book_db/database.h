#ifndef LEARN_BOOK_DB_DATABASE_H
#define LEARN_BOOK_DB_DATABASE_H

#include <stdbool.h>

#include "learn_book_db/error.h"

typedef struct LbdbDatabase LbdbDatabase;

typedef enum LbdbDatabaseMode {
    LBDB_DATABASE_READ_ONLY,
    LBDB_DATABASE_READ_WRITE,
    LBDB_DATABASE_CREATE
} LbdbDatabaseMode;

LbdbError lbdb_database_open(const char *path, LbdbDatabaseMode mode, LbdbDatabase **out_database);
void lbdb_database_close(LbdbDatabase *database);
LbdbError lbdb_database_exec_static(LbdbDatabase *database, const char *sql);
LbdbError lbdb_database_begin_immediate(LbdbDatabase *database);
LbdbError lbdb_database_commit(LbdbDatabase *database);
void lbdb_database_rollback(LbdbDatabase *database);
LbdbError lbdb_database_check_json(LbdbDatabase *database, bool *available);
LbdbError lbdb_database_backup(LbdbDatabase *source, LbdbDatabase *destination);
const char *lbdb_database_message(const LbdbDatabase *database);

#endif
