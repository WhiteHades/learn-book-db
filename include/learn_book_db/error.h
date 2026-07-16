#ifndef LEARN_BOOK_DB_ERROR_H
#define LEARN_BOOK_DB_ERROR_H

typedef enum LbdbError {
    LBDB_OK = 0,
    LBDB_ERROR_USAGE,
    LBDB_ERROR_IO,
    LBDB_ERROR_MEMORY,
    LBDB_ERROR_SQLITE,
    LBDB_ERROR_JSON,
    LBDB_ERROR_VALIDATION,
    LBDB_ERROR_NOT_FOUND,
    LBDB_ERROR_CONFLICT,
    LBDB_ERROR_STATE,
    LBDB_ERROR_UNSUPPORTED,
    LBDB_ERROR_INTERNAL
} LbdbError;

const char *lbdb_error_name(LbdbError error);
int lbdb_error_exit_code(LbdbError error);

#endif
