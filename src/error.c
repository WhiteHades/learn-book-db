#include "learn_book_db/error.h"

const char *lbdb_error_name(LbdbError error) {
    switch (error) {
    case LBDB_OK:
        return "ok";
    case LBDB_ERROR_USAGE:
        return "usage";
    case LBDB_ERROR_IO:
        return "io";
    case LBDB_ERROR_MEMORY:
        return "memory";
    case LBDB_ERROR_SQLITE:
        return "sqlite";
    case LBDB_ERROR_JSON:
        return "json";
    case LBDB_ERROR_VALIDATION:
        return "validation";
    case LBDB_ERROR_NOT_FOUND:
        return "not_found";
    case LBDB_ERROR_CONFLICT:
        return "conflict";
    case LBDB_ERROR_STATE:
        return "invalid_state";
    case LBDB_ERROR_UNSUPPORTED:
        return "unsupported";
    case LBDB_ERROR_INTERNAL:
        return "internal";
    }
    return "internal";
}

int lbdb_error_exit_code(LbdbError error) {
    if (error == LBDB_OK) {
        return 0;
    }
    if (error == LBDB_ERROR_USAGE) {
        return 2;
    }
    return 1;
}
