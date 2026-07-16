#include "learn_book_db/app.h"

#include <stdio.h>

int main(int argc, char **argv) {
    LbdbApp *app = lbdb_app_create(argc, argv);
    int exit_code = 1;
    if (app == NULL) {
        fputs("{\"ok\":false,\"error\":{\"code\":\"memory\",\"message\":\"Unable to "
              "create application\",\"details\":null}}\n",
              stderr);
        return 1;
    }
    exit_code = lbdb_app_run(app, stdout, stderr);
    lbdb_app_destroy(app);
    return exit_code;
}
