#ifndef LEARN_BOOK_DB_COMMAND_H
#define LEARN_BOOK_DB_COMMAND_H

#include "learn_book_db/error.h"

typedef struct LbdbApp LbdbApp;
typedef struct LbdbCommand LbdbCommand;

LbdbError lbdb_command_create(LbdbApp *app, int argc, char *const argv[],
                              LbdbCommand **out_command);
void lbdb_command_destroy(LbdbCommand *command);
LbdbError lbdb_command_execute(LbdbCommand *command);

#endif
