#ifndef LEARN_BOOK_DB_APP_H
#define LEARN_BOOK_DB_APP_H

#include <stdio.h>

#include "learn_book_db/error.h"

typedef struct LbdbApp LbdbApp;

LbdbApp *lbdb_app_create(int argc, char *const argv[]);
void lbdb_app_destroy(LbdbApp *app);
int lbdb_app_run(LbdbApp *app, FILE *standard_output, FILE *standard_error);
LbdbError lbdb_app_error(const LbdbApp *app);
const char *lbdb_app_error_message(const LbdbApp *app);

#endif
