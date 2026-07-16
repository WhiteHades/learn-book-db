#include "learn_book_db/app.h"
#include "learn_book_db/json.h"
#include "learn_book_db/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures += 1;
    }
}

static void test_sha256(void) {
    static const char expected[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    LbdbSha256 context = {0};
    unsigned char digest[32] = {0};
    char output[65] = {0};
    lbdb_sha256_init(&context);
    lbdb_sha256_update(&context, "a", 1U);
    lbdb_sha256_update(&context, "bc", 2U);
    lbdb_sha256_final(&context, digest);
    lbdb_sha256_hex(digest, output);
    check(strcmp(output, expected) == 0, "SHA-256 matches the FIPS abc vector");
}

static void test_json_writer(void) {
    LbdbJsonWriter *writer = lbdb_json_writer_create(false);
    check(writer != NULL, "JSON writer allocates");
    if (writer == NULL) {
        return;
    }
    bool ok = lbdb_json_begin_object(writer);
    ok = ok && lbdb_json_key(writer, "escaped");
    ok = ok && lbdb_json_string(writer, "quote=\" newline=\n tab=\t slash=\\");
    ok = ok && lbdb_json_key(writer, "array");
    ok = ok && lbdb_json_begin_array(writer);
    ok = ok && lbdb_json_int(writer, -7);
    ok = ok && lbdb_json_bool(writer, true);
    ok = ok && lbdb_json_null(writer);
    ok = ok && lbdb_json_end_array(writer);
    ok = ok && lbdb_json_key(writer, "raw");
    ok = ok && lbdb_json_raw(writer, "{\"n\":1}");
    ok = ok && lbdb_json_end_object(writer);
    check(ok, "JSON writer accepts a nested document");
    check(strcmp(lbdb_json_data(writer),
                 "{\"escaped\":\"quote=\\\" newline=\\n tab=\\t slash=\\\\\","
                 "\"array\":[-7,true,null],\"raw\":{\"n\":1}}") == 0,
          "JSON writer escapes and separates values exactly");
    check(lbdb_json_size(writer) == strlen(lbdb_json_data(writer)),
          "JSON writer reports its byte length");
    check(!lbdb_json_key(writer, "invalid"), "JSON writer rejects a key outside an object");
    check(lbdb_json_writer_reset(writer, true), "JSON writer resets after an error");
    ok = lbdb_json_begin_array(writer) && lbdb_json_string(writer, "x") &&
         lbdb_json_end_array(writer);
    check(ok && strstr(lbdb_json_data(writer), "\n") != NULL,
          "Pretty JSON contains formatting whitespace");
    lbdb_json_writer_destroy(writer);
}

static void test_root_path_normalization(void) {
    char *arguments[] = {"learn-book-db", "--root", "/", "--version"};
    LbdbApp *app = lbdb_app_create(4, arguments);
    check(app != NULL, "application accepts the filesystem root as its project root");
    if (app != NULL) {
        check(lbdb_app_error(app) == LBDB_OK, "filesystem-root normalization stays in bounds");
        lbdb_app_destroy(app);
    }
}

int main(void) {
    test_sha256();
    test_json_writer();
    test_root_path_normalization();
    if (failures != 0) {
        fprintf(stderr, "%d unit assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("unit_core: all assertions passed");
    return EXIT_SUCCESS;
}
