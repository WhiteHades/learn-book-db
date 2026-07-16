#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct RunResult {
    int exit_code;
    char *output;
    char *error;
} RunResult;

static int failures = 0;
static sqlite3 *json_database = NULL;

#define COMMAND(...) ((const char *const[]){__VA_ARGS__, NULL})

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures += 1;
    }
}

static char *path_join(const char *parent, const char *child) {
    const size_t parent_size = strlen(parent);
    const size_t child_size = strlen(child);
    if (parent_size > SIZE_MAX - child_size - 2U) {
        return NULL;
    }
    char *path = malloc(parent_size + child_size + 2U);
    if (path != NULL) {
        (void)snprintf(path, parent_size + child_size + 2U, "%s/%s", parent, child);
    }
    return path;
}

static bool write_all(int descriptor, const void *contents, size_t size) {
    const unsigned char *cursor = contents;
    while (size > 0U) {
        const ssize_t written = write(descriptor, cursor, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool write_file(const char *path, const void *contents, size_t size) {
    const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        return false;
    }
    const bool written = write_all(descriptor, contents, size);
    const bool closed = close(descriptor) == 0;
    return written && closed;
}

static char *read_file(const char *path, size_t *size_output) {
    struct stat status = {0};
    if (stat(path, &status) != 0 || status.st_size < 0 ||
        (uintmax_t)status.st_size > SIZE_MAX - 1U) {
        return NULL;
    }
    const size_t size = (size_t)status.st_size;
    char *contents = malloc(size + 1U);
    if (contents == NULL) {
        return NULL;
    }
    const int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        free(contents);
        return NULL;
    }
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t bytes = read(descriptor, contents + offset, size - offset);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(contents);
            (void)close(descriptor);
            return NULL;
        }
        if (bytes == 0) {
            free(contents);
            (void)close(descriptor);
            return NULL;
        }
        offset += (size_t)bytes;
    }
    if (close(descriptor) != 0) {
        free(contents);
        return NULL;
    }
    contents[size] = '\0';
    if (size_output != NULL) {
        *size_output = size;
    }
    return contents;
}

static bool copy_file(const char *source, const char *destination) {
    size_t size = 0U;
    char *contents = read_file(source, &size);
    if (contents == NULL) {
        return false;
    }
    const bool copied = write_file(destination, contents, size);
    free(contents);
    return copied;
}

static char *replace_once(const char *contents, const char *target, const char *replacement) {
    const char *match = strstr(contents, target);
    if (match == NULL) {
        return NULL;
    }
    const size_t prefix_size = (size_t)(match - contents);
    const size_t target_size = strlen(target);
    const size_t replacement_size = strlen(replacement);
    const size_t suffix_size = strlen(match + target_size);
    if (prefix_size > SIZE_MAX - replacement_size ||
        prefix_size + replacement_size > SIZE_MAX - suffix_size - 1U) {
        return NULL;
    }
    char *result = malloc(prefix_size + replacement_size + suffix_size + 1U);
    if (result != NULL) {
        memcpy(result, contents, prefix_size);
        memcpy(result + prefix_size, replacement, replacement_size);
        memcpy(result + prefix_size + replacement_size, match + target_size, suffix_size + 1U);
    }
    return result;
}

static bool sqlite_execute(const char *path, const char *sql) {
    sqlite3 *database = NULL;
    char *message = NULL;
    const int open_result = sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE, NULL);
    const int execute_result =
        open_result == SQLITE_OK ? sqlite3_exec(database, sql, NULL, NULL, &message) : open_result;
    if (execute_result != SQLITE_OK) {
        fprintf(stderr, "FAIL: SQLite fixture statement failed: %s\n",
                message != NULL ? message : sqlite3_errmsg(database));
    }
    sqlite3_free(message);
    sqlite3_close(database);
    return execute_result == SQLITE_OK;
}

static bool sqlite_statement_rejected(const char *path, const char *sql) {
    sqlite3 *database = NULL;
    char *message = NULL;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
        sqlite3_exec(database, "PRAGMA foreign_keys=ON;BEGIN", NULL, NULL, &message) != SQLITE_OK) {
        fprintf(stderr, "FAIL: cannot begin SQLite rejection fixture: %s\n",
                message != NULL ? message : sqlite3_errmsg(database));
        sqlite3_free(message);
        sqlite3_close(database);
        return false;
    }
    sqlite3_free(message);
    message = NULL;
    const bool rejected = sqlite3_exec(database, sql, NULL, NULL, &message) != SQLITE_OK;
    sqlite3_free(message);
    message = NULL;
    if (sqlite3_exec(database, "ROLLBACK", NULL, NULL, &message) != SQLITE_OK) {
        fprintf(stderr, "FAIL: cannot roll back SQLite rejection fixture: %s\n",
                message != NULL ? message : sqlite3_errmsg(database));
    }
    sqlite3_free(message);
    sqlite3_close(database);
    return rejected;
}

static int64_t sqlite_integer(const char *path, const char *sql) {
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    int64_t value = -1;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    } else {
        fprintf(stderr, "FAIL: SQLite fixture query failed: %s\n", sqlite3_errmsg(database));
        failures += 1;
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

static bool remove_tree(const char *path) {
    struct stat status = {0};
    if (lstat(path, &status) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(status.st_mode)) {
        return unlink(path) == 0;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return false;
    }
    bool removed = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                removed = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *child = path_join(path, entry->d_name);
        if (child == NULL || !remove_tree(child)) {
            removed = false;
        }
        free(child);
    }
    if (closedir(directory) != 0) {
        removed = false;
    }
    return removed && rmdir(path) == 0;
}

static bool make_directory(const char *path) { return mkdir(path, 0700) == 0 || errno == EEXIST; }

static bool setup_root(const char *root) {
    static const char manifest[] = "{\n"
                                   "  \"format_version\": 1,\n"
                                   "  \"corpora\": [{\n"
                                   "    \"slug\": \"example-book\",\n"
                                   "    \"units\": [{\n"
                                   "      \"key\": \"chapter-1\",\n"
                                   "      \"type\": \"chapter\",\n"
                                   "      \"path\": \"chapter.md\",\n"
                                   "      \"include_in_quizzes\": true,\n"
                                   "      \"included_reason\": \"Integration fixture.\"\n"
                                   "    }]\n"
                                   "  }]\n"
                                   "}\n";
    char *source = path_join(root, "chapter.md");
    char *bank = path_join(root, "bank.json");
    char *manifest_path = path_join(root, "manifest.json");
    const bool ready = source != NULL && bank != NULL && manifest_path != NULL &&
                       make_directory(root) &&
                       copy_file(LBDB_SOURCE_DIR "/examples/corpus/chapter-1.md", source) &&
                       copy_file(LBDB_SOURCE_DIR "/examples/bank.json", bank) &&
                       write_file(manifest_path, manifest, sizeof(manifest) - 1U);
    free(source);
    free(bank);
    free(manifest_path);
    return ready;
}

static void run_result_destroy(RunResult *result) {
    free(result->output);
    free(result->error);
    *result = (RunResult){0};
}

static RunResult run_cli(const char *root, const char *database, const char *const command[]) {
    RunResult result = {.exit_code = 127};
    char *output_path = path_join(LBDB_TEST_WORK_DIR, "command.stdout");
    char *error_path = path_join(LBDB_TEST_WORK_DIR, "command.stderr");
    char *arguments[64] = {0};
    size_t count = 0U;
    arguments[count++] = (char *)LBDB_CLI_PATH;
    arguments[count++] = (char *)"--root";
    arguments[count++] = (char *)root;
    arguments[count++] = (char *)"--db";
    arguments[count++] = (char *)database;
    arguments[count++] = (char *)"--manifest";
    arguments[count++] = (char *)"manifest.json";
    for (size_t index = 0U; command[index] != NULL; ++index) {
        if (count + 1U >= sizeof(arguments) / sizeof(arguments[0])) {
            check(false, "integration command exceeds the argument limit");
            free(output_path);
            free(error_path);
            return result;
        }
        arguments[count++] = (char *)command[index];
    }
    arguments[count] = NULL;
    if (output_path == NULL || error_path == NULL) {
        check(false, "capture paths allocate");
        free(output_path);
        free(error_path);
        return result;
    }
    const pid_t child = fork();
    if (child == 0) {
        const int output = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int error = open(error_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || error < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(error, STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(output);
        (void)close(error);
        execv(LBDB_CLI_PATH, arguments);
        _exit(127);
    }
    if (child < 0) {
        check(false, "fork succeeds");
        free(output_path);
        free(error_path);
        return result;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            check(false, "waitpid succeeds");
            free(output_path);
            free(error_path);
            return result;
        }
    }
    result.exit_code = WIFEXITED(status)     ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                             : 127;
    result.output = read_file(output_path, NULL);
    result.error = read_file(error_path, NULL);
    check(result.output != NULL && result.error != NULL, "command captures both streams");
    free(output_path);
    free(error_path);
    return result;
}

static bool json_valid(const char *json) {
    sqlite3_stmt *statement = NULL;
    bool valid = false;
    if (json != NULL &&
        sqlite3_prepare_v2(json_database, "SELECT json_valid(?1)", -1, &statement, NULL) ==
            SQLITE_OK &&
        sqlite3_bind_text(statement, 1, json, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        valid = sqlite3_column_int(statement, 0) == 1;
    }
    sqlite3_finalize(statement);
    return valid;
}

static sqlite3_stmt *json_value(const char *json, const char *path) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(json_database, "SELECT json_type(?1,?2),json_extract(?1,?2)", -1,
                           &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, json, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return NULL;
    }
    return statement;
}

static int64_t json_integer(const char *json, const char *path) {
    sqlite3_stmt *statement = json_value(json, path);
    if (statement == NULL || sqlite3_column_type(statement, 1) != SQLITE_INTEGER) {
        fprintf(stderr, "FAIL: JSON integer is missing at %s\n", path);
        failures += 1;
        sqlite3_finalize(statement);
        return 0;
    }
    const int64_t value = sqlite3_column_int64(statement, 1);
    sqlite3_finalize(statement);
    return value;
}

static int64_t json_array_length(const char *json, const char *path) {
    sqlite3_stmt *statement = NULL;
    int64_t length = -1;
    if (sqlite3_prepare_v2(json_database, "SELECT json_array_length(?1,?2)", -1, &statement,
                           NULL) == SQLITE_OK &&
        sqlite3_bind_text(statement, 1, json, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW &&
        sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
        length = sqlite3_column_int64(statement, 0);
    } else {
        fprintf(stderr, "FAIL: JSON array is missing at %s\n", path);
        failures += 1;
    }
    sqlite3_finalize(statement);
    return length;
}

static void expect_json_integer(const char *json, const char *path, int64_t expected) {
    const int64_t actual = json_integer(json, path);
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s expected %lld, received %lld\n", path, (long long)expected,
                (long long)actual);
        failures += 1;
    }
}

static void expect_json_text(const char *json, const char *path, const char *expected) {
    sqlite3_stmt *statement = json_value(json, path);
    const char *actual = statement != NULL ? (const char *)sqlite3_column_text(statement, 1) : NULL;
    if (actual == NULL || strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: %s expected \"%s\", received \"%s\"\n", path, expected,
                actual != NULL ? actual : "(null)");
        failures += 1;
    }
    sqlite3_finalize(statement);
}

static void expect_json_absent(const char *json, const char *path) {
    sqlite3_stmt *statement = json_value(json, path);
    if (statement == NULL || sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        fprintf(stderr, "FAIL: JSON path should be absent: %s\n", path);
        failures += 1;
    }
    sqlite3_finalize(statement);
}

static RunResult expect_success(const char *root, const char *database,
                                const char *expected_command, const char *const command[]) {
    RunResult result = run_cli(root, database, command);
    if (result.exit_code != 0) {
        fprintf(stderr, "FAIL: %s exited %d: %s\n", expected_command, result.exit_code,
                result.error != NULL ? result.error : "(no error output)");
        failures += 1;
        return result;
    }
    check(result.error != NULL && result.error[0] == '\0',
          "successful command keeps standard error empty");
    check(json_valid(result.output), "successful command emits one valid JSON document");
    expect_json_integer(result.output, "$.ok", 1);
    expect_json_text(result.output, "$.command", expected_command);
    return result;
}

static RunResult expect_failure(const char *root, const char *database, const char *expected_code,
                                const char *const command[]) {
    RunResult result = run_cli(root, database, command);
    check(result.exit_code != 0, "invalid command exits nonzero");
    check(result.output != NULL && result.output[0] == '\0',
          "failed command keeps standard output empty");
    check(json_valid(result.error), "failed command emits one valid JSON document");
    expect_json_integer(result.error, "$.ok", 0);
    expect_json_text(result.error, "$.error.code", expected_code);
    return result;
}

static void format_id(char output[32], int64_t value) {
    const int length = snprintf(output, 32U, "%lld", (long long)value);
    check(length > 0 && length < 32, "integer ID formats");
}

static bool files_equal(const char *left_path, const char *right_path) {
    size_t left_size = 0U;
    size_t right_size = 0U;
    char *left = read_file(left_path, &left_size);
    char *right = read_file(right_path, &right_size);
    const bool equal = left != NULL && right != NULL && left_size == right_size &&
                       memcmp(left, right, left_size) == 0;
    free(left);
    free(right);
    return equal;
}

static bool test_strict_exchange_formats(const char *root) {
    static const char *const manifest_database = "manifest-format.db";
    char *manifest_path = path_join(root, "manifest.json");
    char *bank_path = path_join(root, "bank.json");
    char *missing_bank_path = path_join(root, "bank-missing-format.json");
    char *future_bank_path = path_join(root, "bank-future-format.json");
    char *provenance_bank_path = path_join(root, "bank-bad-provenance.json");
    size_t manifest_size = 0U;
    size_t bank_size = 0U;
    char *manifest = manifest_path != NULL ? read_file(manifest_path, &manifest_size) : NULL;
    char *bank = bank_path != NULL ? read_file(bank_path, &bank_size) : NULL;
    char *missing_manifest =
        manifest != NULL ? replace_once(manifest, "  \"format_version\": 1,\n", "") : NULL;
    char *future_manifest =
        manifest != NULL ? replace_once(manifest, "\"format_version\": 1", "\"format_version\": 2")
                         : NULL;
    char *missing_bank = bank != NULL ? replace_once(bank, "  \"format_version\": 1,\n", "") : NULL;
    char *future_bank =
        bank != NULL ? replace_once(bank, "\"format_version\": 1", "\"format_version\": 2") : NULL;
    char *bad_provenance =
        bank != NULL ? replace_once(bank, "\"source_line_start\": 3", "\"source_line_start\": 4")
                     : NULL;
    (void)bank_size;
    check(manifest_path != NULL && bank_path != NULL && missing_bank_path != NULL &&
              future_bank_path != NULL && provenance_bank_path != NULL && manifest != NULL &&
              bank != NULL && missing_manifest != NULL && future_manifest != NULL &&
              missing_bank != NULL && future_bank != NULL && bad_provenance != NULL,
          "strict exchange-format fixtures allocate");

    RunResult result = expect_success(root, manifest_database, "db.init", COMMAND("db", "init"));
    run_result_destroy(&result);
    check(write_file(manifest_path, missing_manifest, strlen(missing_manifest)),
          "manifest without a format version is written");
    result = expect_failure(root, manifest_database, "validation", COMMAND("corpus", "sync"));
    run_result_destroy(&result);
    check(write_file(manifest_path, future_manifest, strlen(future_manifest)),
          "future manifest version is written");
    result = expect_failure(root, manifest_database, "unsupported", COMMAND("corpus", "sync"));
    run_result_destroy(&result);
    check(write_file(manifest_path, manifest, manifest_size), "valid manifest is restored");

    static const char *const bank_databases[] = {"bank-missing-format.db", "bank-future-format.db",
                                                 "bank-bad-provenance.db"};
    const char *const bank_files[] = {"bank-missing-format.json", "bank-future-format.json",
                                      "bank-bad-provenance.json"};
    const char *const expected_errors[] = {"validation", "unsupported", "validation"};
    check(write_file(missing_bank_path, missing_bank, strlen(missing_bank)) &&
              write_file(future_bank_path, future_bank, strlen(future_bank)) &&
              write_file(provenance_bank_path, bad_provenance, strlen(bad_provenance)),
          "invalid bank fixtures are written");
    for (size_t index = 0U; index < 3U; ++index) {
        result = expect_success(root, bank_databases[index], "db.init", COMMAND("db", "init"));
        run_result_destroy(&result);
        result =
            expect_success(root, bank_databases[index], "corpus.sync", COMMAND("corpus", "sync"));
        run_result_destroy(&result);
        result = expect_failure(root, bank_databases[index], expected_errors[index],
                                COMMAND("bank", "import", bank_files[index]));
        run_result_destroy(&result);
    }

    free(manifest_path);
    free(bank_path);
    free(missing_bank_path);
    free(future_bank_path);
    free(provenance_bank_path);
    free(manifest);
    free(bank);
    free(missing_manifest);
    free(future_manifest);
    free(missing_bank);
    free(future_bank);
    free(bad_provenance);
    return failures == 0;
}

static void check_terminal_snapshot_guards(const char *root, const char *database,
                                           int64_t quiz_id) {
    char *database_path = path_join(root, database);
    char sql[1024] = {0};
    check(database_path != NULL, "terminal snapshot database path allocates");
#define EXPECT_TERMINAL_REJECTION(message, ...)                                                    \
    do {                                                                                           \
        const int length = snprintf(sql, sizeof(sql), __VA_ARGS__);                                \
        check(length > 0 && (size_t)length < sizeof(sql) &&                                        \
                  sqlite_statement_rejected(database_path, sql),                                   \
              (message));                                                                          \
    } while (false)
    EXPECT_TERMINAL_REJECTION(
        "terminal quiz rejects objective inserts",
        "INSERT INTO quiz_objectives(quiz_id,position,section_title,concept,importance,"
        "source_pages,source_line_start,source_line_end,justification,concept_id) SELECT quiz_id,"
        "position+1000,section_title,concept,importance,source_pages,source_line_start,"
        "source_line_end,justification,concept_id FROM quiz_objectives WHERE quiz_id=%lld LIMIT 1",
        (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION("terminal quiz rejects objective updates",
                              "UPDATE quiz_objectives SET concept=concept WHERE quiz_id=%lld",
                              (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION("terminal quiz rejects objective deletes",
                              "DELETE FROM quiz_objectives WHERE quiz_id=%lld", (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION(
        "terminal quiz rejects question inserts",
        "INSERT INTO quiz_questions(quiz_id,objective_id,position,origin,question_type,"
        "response_format,prompt,options_json,expected_answer,grading_criteria_json,"
        "answer_justification,source_section,source_pages,source_line_start,source_line_end,state,"
        "asked_at,bank_question_id) SELECT quiz_id,objective_id,position+1000,origin,question_type,"
        "response_format,prompt,options_json,expected_answer,grading_criteria_json,"
        "answer_justification,source_section,source_pages,source_line_start,source_line_end,state,"
        "asked_at,bank_question_id FROM quiz_questions WHERE quiz_id=%lld LIMIT 1",
        (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION("terminal quiz rejects question updates",
                              "UPDATE quiz_questions SET prompt=prompt WHERE quiz_id=%lld",
                              (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION("terminal quiz rejects question deletes",
                              "DELETE FROM quiz_questions WHERE quiz_id=%lld", (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION(
        "terminal quiz rejects response inserts",
        "INSERT INTO quiz_responses(question_id,attempt_number,answered_at,answer,assessment,"
        "feedback,metadata_json) SELECT r.question_id,r.attempt_number+1000,r.answered_at,r.answer,"
        "r.assessment,r.feedback,r.metadata_json FROM quiz_responses r JOIN quiz_questions q ON "
        "q.id=r.question_id WHERE q.quiz_id=%lld LIMIT 1",
        (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION(
        "terminal quiz rejects response updates",
        "UPDATE quiz_responses SET feedback=feedback WHERE question_id IN(SELECT id FROM "
        "quiz_questions WHERE quiz_id=%lld)",
        (long long)quiz_id);
    EXPECT_TERMINAL_REJECTION(
        "terminal quiz rejects response deletes",
        "DELETE FROM quiz_responses WHERE question_id IN(SELECT id FROM quiz_questions WHERE "
        "quiz_id=%lld)",
        (long long)quiz_id);
#undef EXPECT_TERMINAL_REJECTION
    free(database_path);
}

static bool test_database_and_bank(const char *root, const char *roundtrip_root) {
    static const char *const database = ".book-learning/learning.db";
    RunResult result = expect_success(root, database, "help", COMMAND("--help"));
    check(json_array_length(result.output, "$.commands") == 46, "help lists every fixed command");
    check(strstr(result.output, "bank.import") != NULL &&
              strstr(result.output, "report.state-drift") != NULL &&
              strstr(result.output, "db.upgrade") == NULL,
          "help lists only the fixed command surface");
    expect_json_text(result.output, "$.commands[0].usage", "bank activate ID");
    run_result_destroy(&result);

    result = expect_success(root, database, "help", COMMAND("--help", "db", "init"));
    expect_json_text(result.output, "$.name", "db.init");
    expect_json_text(result.output, "$.usage", "db init");
    expect_json_absent(result.output, "$.commands");
    run_result_destroy(&result);

    result = expect_success(root, database, "version", COMMAND("--version"));
    expect_json_text(result.output, "$.name", "learn-book-db");
    expect_json_text(result.output, "$.version", "1.0.0");
    expect_json_integer(result.output, "$.schema_version", 1);
    run_result_destroy(&result);

    result = expect_failure(root, database, "usage", COMMAND("sql", "query"));
    run_result_destroy(&result);
    result = expect_success(root, database, "db.init", COMMAND("db", "init"));
    expect_json_integer(result.output, "$.schema_version", 1);
    run_result_destroy(&result);
    result = expect_failure(root, database, "conflict", COMMAND("db", "init"));
    run_result_destroy(&result);

    result = expect_success(root, database, "db.doctor", COMMAND("db", "doctor"));
    expect_json_integer(result.output, "$.healthy", 1);
    expect_json_integer(result.output, "$.checks.json_functions", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "db.status", COMMAND("db", "status"));
    expect_json_integer(result.output, "$.versions.schema", 1);
    run_result_destroy(&result);
    result = expect_failure(root, database, "usage", COMMAND("db", "upgrade"));
    run_result_destroy(&result);

    result = expect_success(root, database, "corpus.sync", COMMAND("corpus", "sync"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "corpus.status", COMMAND("corpus", "status"));
    expect_json_integer(result.output, "$.in_sync", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "bank.validate",
                            COMMAND("bank", "validate", "--allow-incomplete"));
    expect_json_integer(result.output, "$.allow_incomplete", 1);
    run_result_destroy(&result);

    result = expect_success(root, database, "bank.import", COMMAND("bank", "import", "bank.json"));
    expect_json_integer(result.output, "$.concepts", 2);
    expect_json_integer(result.output, "$.questions", 3);
    run_result_destroy(&result);
    result = expect_failure(root, database, "conflict", COMMAND("bank", "import", "bank.json"));
    run_result_destroy(&result);
    result = expect_failure(root, database, "validation", COMMAND("bank", "validate"));
    run_result_destroy(&result);

    result = expect_success(root, database, "bank.search",
                            COMMAND("bank", "search", "--unit", "example-book/chapter-1", "--tag",
                                    "theme:llm-basics", "--tag", "topic:input-output", "--tag-kind",
                                    "mode", "--question-type", "recall", "--response-format",
                                    "free_response", "--active", "all", "--text", "input",
                                    "--limit", "5"));
    expect_json_integer(result.output, "$.count", 1);
    const int64_t first_question = json_integer(result.output, "$.questions[0].id");
    run_result_destroy(&result);
    char first_question_text[32] = {0};
    format_id(first_question_text, first_question);
    result =
        expect_success(root, database, "bank.show", COMMAND("bank", "show", first_question_text));
    expect_json_text(result.output, "$.question.expected_answer",
                     "An input is supplied to the model; an output is produced by the model.");
    run_result_destroy(&result);

    result = expect_success(root, database, "bank.search",
                            COMMAND("bank", "search", "--question-type", "misconception"));
    const int64_t second_question = json_integer(result.output, "$.questions[0].id");
    run_result_destroy(&result);
    char second_question_text[32] = {0};
    format_id(second_question_text, second_question);

    char *all_exports = path_join(root, "all-exports");
    char *exported = path_join(root, "exported.json");
    char *roundtrip_export = path_join(roundtrip_root, "roundtrip.json");
    check(all_exports != NULL && exported != NULL && roundtrip_export != NULL &&
              make_directory(all_exports),
          "export fixture paths are ready");
    result = expect_success(
        root, database, "bank.export",
        COMMAND("bank", "export", "--unit", "chapter-1", "--output", "exported.json"));
    expect_json_integer(result.output, "$.questions", 3);
    run_result_destroy(&result);
    result = expect_success(root, database, "bank.export-all",
                            COMMAND("bank", "export-all", "--output-dir", "all-exports"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);

    result = expect_success(roundtrip_root, database, "db.init", COMMAND("db", "init"));
    run_result_destroy(&result);
    result = expect_success(roundtrip_root, database, "corpus.sync", COMMAND("corpus", "sync"));
    run_result_destroy(&result);
    result = expect_success(roundtrip_root, database, "bank.import",
                            COMMAND("bank", "import", exported));
    run_result_destroy(&result);
    result = expect_success(
        roundtrip_root, database, "bank.export",
        COMMAND("bank", "export", "--unit", "chapter-1", "--output", "roundtrip.json"));
    run_result_destroy(&result);
    check(files_equal(exported, roundtrip_export), "bank export/import/export is byte-stable");

    char *source_path = path_join(root, "chapter.md");
    size_t source_size = 0U;
    char *source_contents = source_path != NULL ? read_file(source_path, &source_size) : NULL;
    static const char addition[] = "\nChanged after bank import.\n";
    char *changed_source = source_contents != NULL ? malloc(source_size + sizeof(addition)) : NULL;
    if (changed_source != NULL) {
        memcpy(changed_source, source_contents, source_size);
        memcpy(changed_source + source_size, addition, sizeof(addition));
    }
    check(changed_source != NULL &&
              write_file(source_path, changed_source, source_size + sizeof(addition) - 1U),
          "source drift fixture is written");
    result = expect_success(root, database, "corpus.status", COMMAND("corpus", "status"));
    expect_json_integer(result.output, "$.in_sync", 0);
    run_result_destroy(&result);
    result = expect_failure(root, database, "conflict", COMMAND("corpus", "sync"));
    run_result_destroy(&result);
    check(source_contents != NULL && write_file(source_path, source_contents, source_size),
          "source drift fixture is restored");
    free(changed_source);
    free(source_contents);
    free(source_path);

    static const char revision[] = "{\"prompt\":\"Revised: what distinguishes input from output?\","
                                   "\"reason\":\"Clarify the wording.\"}";
    result = expect_success(root, database, "bank.revise",
                            COMMAND("bank", "revise", first_question_text, "--input", revision));
    expect_json_integer(result.output, "$.revision", 2);
    run_result_destroy(&result);
    result =
        expect_success(root, database, "bank.show", COMMAND("bank", "show", first_question_text));
    expect_json_text(result.output, "$.question.prompt",
                     "Revised: what distinguishes input from output?");
    run_result_destroy(&result);
    result = expect_success(
        root, database, "bank.retire",
        COMMAND("bank", "retire", second_question_text, "--reason", "Exercise retirement."));
    run_result_destroy(&result);
    result = expect_success(root, database, "bank.search",
                            COMMAND("bank", "search", "--active", "retired"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "bank.activate",
                            COMMAND("bank", "activate", second_question_text));
    run_result_destroy(&result);

    result =
        expect_success(root, database, "tag.alias-add",
                       COMMAND("tag", "alias-add", "--tag", "topic:input-output", "--alias", "io"));
    run_result_destroy(&result);
    result =
        expect_failure(root, database, "conflict",
                       COMMAND("tag", "alias-add", "--tag", "topic:input-output", "--alias", "io"));
    run_result_destroy(&result);
    result = expect_success(root, database, "tag.list",
                            COMMAND("tag", "list", "--kind", "topic", "--search", "input"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "tag.relation-add",
                            COMMAND("tag", "relation-add", "--parent", "theme:llm-basics",
                                    "--child", "io", "--type", "contains"));
    run_result_destroy(&result);
    result = expect_failure(root, database, "conflict",
                            COMMAND("tag", "relation-add", "--parent", "theme:llm-basics",
                                    "--child", "io", "--type", "contains"));
    run_result_destroy(&result);
    result = expect_success(root, database, "tag.relation-remove",
                            COMMAND("tag", "relation-remove", "--parent", "theme:llm-basics",
                                    "--child", "io", "--type", "contains"));
    run_result_destroy(&result);
    result = expect_success(
        root, database, "tag.alias-remove",
        COMMAND("tag", "alias-remove", "--tag", "topic:input-output", "--alias", "io"));
    run_result_destroy(&result);

    result = expect_success(root, database, "template.rebuild", COMMAND("template", "rebuild"));
    expect_json_integer(result.output, "$.count", 7);
    run_result_destroy(&result);
    result = expect_success(root, database, "template.list",
                            COMMAND("template", "list", "--scope", "final", "--unit", "chapter-1",
                                    "--active", "active"));
    expect_json_integer(result.output, "$.count", 1);
    const int64_t final_template = json_integer(result.output, "$.templates[0].id");
    run_result_destroy(&result);
    char final_template_text[32] = {0};
    format_id(final_template_text, final_template);
    result = expect_success(root, database, "template.show",
                            COMMAND("template", "show", final_template_text));
    expect_json_integer(result.output, "$.template.question_count", 3);
    expect_json_absent(result.output, "$.questions[0].expected_answer");
    run_result_destroy(&result);
    result = expect_success(root, database, "bank.validate", COMMAND("bank", "validate"));
    expect_json_integer(result.output, "$.valid", 1);
    run_result_destroy(&result);

    result =
        expect_failure(root, database, "validation",
                       COMMAND("quiz", "start", "--template", final_template_text, "--limit", "1"));
    run_result_destroy(&result);
    result = expect_success(root, database, "quiz.start",
                            COMMAND("quiz", "start", "--template", final_template_text));
    expect_json_integer(result.output, "$.created", 1);
    expect_json_integer(result.output, "$.session.base_question_count", 3);
    const int64_t quiz_id = json_integer(result.output, "$.session.id");
    run_result_destroy(&result);
    char quiz_text[32] = {0};
    format_id(quiz_text, quiz_id);
    char *main_database_path = path_join(root, database);
    check(main_database_path != NULL, "main database path allocates");
    check(sqlite_integer(main_database_path,
                         "SELECT count(*) FROM quiz_events WHERE event_type='session_planned'") ==
              1,
          "quiz start records the planned state");
    check(sqlite_integer(main_database_path,
                         "SELECT count(*) FROM quiz_events WHERE event_type='session_started' AND "
                         "json_extract(payload_json,'$.from')='planned' AND "
                         "json_extract(payload_json,'$.to')='in_progress'") == 1,
          "quiz start records the planned-to-in-progress transition");
    check(
        sqlite_integer(main_database_path,
                       "SELECT count(*) FROM sqlite_schema WHERE type='index' AND name IN("
                       "'source_units_corpus_position','source_sections_unit_position',"
                       "'concepts_unit_position','tags_kind_name','quiz_template_questions_order',"
                       "'quiz_objectives_quiz','quiz_responses_question')") == 0,
        "schema omits indexes duplicated by UNIQUE constraints");
    result = expect_success(root, database, "quiz.start",
                            COMMAND("quiz", "start", "--template", final_template_text));
    expect_json_integer(result.output, "$.created", 0);
    run_result_destroy(&result);
    static const char post_snapshot_revision[] =
        "{\"prompt\":\"Bank wording changed after the quiz snapshot.\","
        "\"reason\":\"Verify snapshot immutability.\"}";
    result = expect_success(
        root, database, "bank.revise",
        COMMAND("bank", "revise", first_question_text, "--input", post_snapshot_revision));
    expect_json_integer(result.output, "$.revision", 3);
    run_result_destroy(&result);
    result =
        expect_success(root, database, "bank.show", COMMAND("bank", "show", first_question_text));
    expect_json_text(result.output, "$.question.prompt",
                     "Bank wording changed after the quiz snapshot.");
    run_result_destroy(&result);
    result = expect_success(
        root, database, "quiz.list",
        COMMAND("quiz", "list", "--status", "in_progress", "--scope", "chapter_final"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "quiz.status", COMMAND("quiz", "status", quiz_text));
    expect_json_absent(result.output, "$.current_question.expected_answer");
    run_result_destroy(&result);
    result = expect_success(root, database, "report.active", COMMAND("report", "active"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);

    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    const int64_t quiz_question_one = json_integer(result.output, "$.question.id");
    expect_json_text(result.output, "$.question.prompt",
                     "Revised: what distinguishes input from output?");
    expect_json_absent(result.output, "$.question.expected_answer");
    run_result_destroy(&result);
    char quiz_question_one_text[32] = {0};
    format_id(quiz_question_one_text, quiz_question_one);
    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    expect_json_integer(result.output, "$.already_asked", 1);
    expect_json_integer(result.output, "$.question.id", quiz_question_one);
    run_result_destroy(&result);
    result = expect_success(
        root, database, "response.submit",
        COMMAND("response", "submit", "--question", quiz_question_one_text, "--answer",
                "They are similar.", "--assessment", "incorrect", "--feedback",
                "Distinguish supplied from produced information.", "--learning-topic",
                "Input and output", "--learning-status", "learning", "--evidence",
                "The first attempt confused the terms.", "--next-step", "Explain both terms."));
    expect_json_integer(result.output, "$.attempt_number", 1);
    expect_json_integer(result.output, "$.learning_record_id", 1);
    expect_json_absent(result.output, "$.expected_answer");
    run_result_destroy(&result);
    result = expect_success(root, database, "response.submit",
                            COMMAND("response", "submit", "--question", quiz_question_one_text,
                                    "--answer", "Input is supplied and output is produced.",
                                    "--assessment", "correct", "--feedback",
                                    "Both directions are now correct."));
    expect_json_integer(result.output, "$.attempt_number", 2);
    run_result_destroy(&result);

    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    const int64_t quiz_question_two = json_integer(result.output, "$.question.id");
    run_result_destroy(&result);
    char quiz_question_two_text[32] = {0};
    format_id(quiz_question_two_text, quiz_question_two);
    result = expect_success(root, database, "quiz.defer",
                            COMMAND("quiz", "defer", quiz_text, "--question",
                                    quiz_question_two_text, "--reason", "Review later."));
    expect_json_text(result.output, "$.state", "deferred");
    run_result_destroy(&result);
    result =
        expect_success(root, database, "quiz.requeue",
                       COMMAND("quiz", "requeue", quiz_text, "--question", quiz_question_two_text));
    expect_json_text(result.output, "$.state", "planned");
    run_result_destroy(&result);
    result = expect_success(root, database, "quiz.pause",
                            COMMAND("quiz", "pause", quiz_text, "--reason", "Take a break."));
    expect_json_text(result.output, "$.session.state", "paused");
    run_result_destroy(&result);
    result = expect_failure(root, database, "invalid_state", COMMAND("quiz", "next", quiz_text));
    run_result_destroy(&result);
    result = expect_success(root, database, "quiz.resume", COMMAND("quiz", "resume", quiz_text));
    expect_json_text(result.output, "$.session.state", "in_progress");
    run_result_destroy(&result);

    static const char follow_up[] =
        "{\"question_type\":\"recall\",\"response_format\":\"free_response\","
        "\"prompt\":\"Which direction does model output travel?\","
        "\"expected_answer\":\"It travels from the model to the caller.\","
        "\"grading_criteria\":[\"States that output leaves the model.\"],"
        "\"answer_justification\":\"The source defines output as produced by the model.\","
        "\"source_section\":\"1.1 Inputs and outputs\",\"source_pages\":\"0001\","
        "\"source_line_start\":7,\"source_line_end\":8,\"objective_position\":1}";
    result = expect_success(root, database, "quiz.follow-up",
                            COMMAND("quiz", "follow-up", quiz_text, "--input", follow_up));
    const int64_t follow_up_question = json_integer(result.output, "$.question_id");
    expect_json_integer(result.output, "$.progress.follow_up_questions", 1);
    run_result_destroy(&result);
    char follow_up_question_text[32] = {0};
    format_id(follow_up_question_text, follow_up_question);
    result =
        expect_failure(root, database, "invalid_state", COMMAND("quiz", "complete", quiz_text));
    run_result_destroy(&result);

    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    expect_json_integer(result.output, "$.question.id", quiz_question_two);
    run_result_destroy(&result);
    result = expect_success(root, database, "response.submit",
                            COMMAND("response", "submit", "--question", quiz_question_two_text,
                                    "--answer", "They stay unchanged.", "--assessment", "correct",
                                    "--feedback", "Correctly rejects retraining during generation.",
                                    "--reveal-answer"));
    const int64_t response_to_regrade = json_integer(result.output, "$.response_id");
    expect_json_text(result.output, "$.expected_answer", "They stay unchanged.");
    run_result_destroy(&result);
    char response_to_regrade_text[32] = {0};
    format_id(response_to_regrade_text, response_to_regrade);
    result = expect_success(root, database, "response.regrade",
                            COMMAND("response", "regrade", "--response", response_to_regrade_text,
                                    "--assessment", "partially_correct", "--feedback",
                                    "Correct choice, but no explanation was provided.", "--reason",
                                    "Apply the explanation rubric consistently."));
    expect_json_text(result.output, "$.after.assessment", "partially_correct");
    run_result_destroy(&result);

    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    const int64_t quiz_question_three = json_integer(result.output, "$.question.id");
    run_result_destroy(&result);
    char quiz_question_three_text[32] = {0};
    format_id(quiz_question_three_text, quiz_question_three);
    result = expect_success(
        root, database, "response.submit",
        COMMAND("response", "submit", "--question", quiz_question_three_text, "--answer",
                "Training, because parameters change from examples and feedback.", "--assessment",
                "correct", "--feedback", "Correct process and reason."));
    run_result_destroy(&result);
    result = expect_success(root, database, "quiz.next", COMMAND("quiz", "next", quiz_text));
    expect_json_integer(result.output, "$.question.id", follow_up_question);
    run_result_destroy(&result);
    result = expect_success(root, database, "response.submit",
                            COMMAND("response", "submit", "--question", follow_up_question_text,
                                    "--answer", "It leaves the model.", "--assessment", "correct",
                                    "--feedback", "The output direction is correct."));
    run_result_destroy(&result);
    result =
        expect_success(root, database, "quiz.complete", COMMAND("quiz", "complete", quiz_text));
    expect_json_text(result.output, "$.session.state", "completed");
    run_result_destroy(&result);

    result = expect_failure(root, database, "invalid_state", COMMAND("quiz", "pause", quiz_text));
    run_result_destroy(&result);
    result = expect_failure(root, database, "invalid_state",
                            COMMAND("quiz", "follow-up", quiz_text, "--input", follow_up));
    run_result_destroy(&result);
    result = expect_failure(root, database, "invalid_state",
                            COMMAND("response", "submit", "--question", quiz_question_one_text,
                                    "--answer", "No terminal mutation.", "--assessment", "correct",
                                    "--feedback", "This must be rejected."));
    run_result_destroy(&result);
    result = expect_success(root, database, "report.quiz", COMMAND("report", "quiz", quiz_text));
    expect_json_text(result.output, "$.session.state", "completed");
    expect_json_integer(result.output, "$.session.progress.total_questions", 4);
    run_result_destroy(&result);
    check_terminal_snapshot_guards(root, database, quiz_id);

    result = expect_success(
        root, database, "learning.add",
        COMMAND("learning", "add", "--topic", "Training", "--status", "review", "--evidence",
                "Correctly identified a parameter-changing scenario.", "--next-step",
                "Contrast training with generation.", "--source-type", "conversation"));
    const int64_t learning_record = json_integer(result.output, "$.record.id");
    run_result_destroy(&result);
    char learning_record_text[32] = {0};
    format_id(learning_record_text, learning_record);
    result = expect_success(root, database, "learning.list",
                            COMMAND("learning", "list", "--status", "learning", "--topic", "Input",
                                    "--quiz", quiz_text, "--limit", "5"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "learning.show",
                            COMMAND("learning", "show", learning_record_text));
    expect_json_text(result.output, "$.record.topic", "Training");
    run_result_destroy(&result);

    result = expect_success(root, database, "report.coverage", COMMAND("report", "coverage"));
    expect_json_integer(result.output, "$.units[0].concept_count", 2);
    run_result_destroy(&result);
    result = expect_success(root, database, "report.mastery", COMMAND("report", "mastery"));
    check(strstr(result.output, "Input and output") != NULL &&
              strstr(result.output, "Training") != NULL,
          "mastery report contains latest topic evidence");
    run_result_destroy(&result);
    result = expect_success(root, database, "report.state-drift", COMMAND("report", "state-drift"));
    expect_json_integer(result.output, "$.in_sync", 1);
    run_result_destroy(&result);

    char *learning_path = path_join(root, ".book-learning/LEARNING.md");
    char drift_line[128] = {0};
    const int drift_length = snprintf(drift_line, sizeof(drift_line),
                                      "# State\nquiz_id=%lld paused\n", (long long)quiz_id);
    check(learning_path != NULL && drift_length > 0 && (size_t)drift_length < sizeof(drift_line) &&
              write_file(learning_path, drift_line, (size_t)drift_length),
          "state-drift fixture is written");
    result = expect_success(root, database, "report.state-drift", COMMAND("report", "state-drift"));
    expect_json_integer(result.output, "$.in_sync", 0);
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    const int sync_length = snprintf(drift_line, sizeof(drift_line),
                                     "# State\nquiz_id=%lld completed\n", (long long)quiz_id);
    check(sync_length > 0 && (size_t)sync_length < sizeof(drift_line) &&
              write_file(learning_path, drift_line, (size_t)sync_length),
          "state-drift fixture is synchronized");
    free(learning_path);
    result = expect_success(root, database, "report.state-drift", COMMAND("report", "state-drift"));
    expect_json_integer(result.output, "$.in_sync", 1);
    run_result_destroy(&result);

    result = expect_success(
        root, database, "template.list",
        COMMAND("template", "list", "--scope", "topic", "--tag", "topic:input-output"));
    const int64_t topic_template = json_integer(result.output, "$.templates[0].id");
    run_result_destroy(&result);
    char topic_template_text[32] = {0};
    format_id(topic_template_text, topic_template);
    result =
        expect_success(root, database, "quiz.start",
                       COMMAND("quiz", "start", "--template", topic_template_text, "--limit", "1"));
    expect_json_integer(result.output, "$.session.base_question_count", 1);
    const int64_t second_quiz = json_integer(result.output, "$.session.id");
    run_result_destroy(&result);
    char second_quiz_text[32] = {0};
    format_id(second_quiz_text, second_quiz);
    static const char legacy_follow_up[] =
        "{\"type\":\"recall\",\"format\":\"free_response\","
        "\"prompt\":\"Legacy aliases must be rejected.\","
        "\"expected_answer\":\"They are not canonical fields.\","
        "\"grading_criteria\":[\"Rejects aliases.\"],"
        "\"answer_justification\":\"Canonical input names are required.\","
        "\"source_section\":\"1.1 Inputs and outputs\",\"source_pages\":\"0001\","
        "\"source_line_start\":7,\"source_line_end\":8,\"objective_position\":1}";
    result =
        expect_failure(root, database, "validation",
                       COMMAND("quiz", "follow-up", second_quiz_text, "--input", legacy_follow_up));
    run_result_destroy(&result);
    result = expect_success(
        root, database, "quiz.abandon",
        COMMAND("quiz", "abandon", second_quiz_text, "--reason", "Exercise terminal abandonment."));
    expect_json_text(result.output, "$.session.state", "abandoned");
    run_result_destroy(&result);
    char abandoned_state_sql[256] = {0};
    (void)snprintf(abandoned_state_sql, sizeof(abandoned_state_sql),
                   "SELECT count(*) FROM quiz_questions WHERE quiz_id=%lld AND state<>'retired'",
                   (long long)second_quiz);
    check(sqlite_integer(main_database_path, abandoned_state_sql) == 0,
          "abandon retires every unresolved question");
    (void)snprintf(abandoned_state_sql, sizeof(abandoned_state_sql),
                   "SELECT count(*) FROM quiz_events WHERE quiz_id=%lld AND "
                   "event_type='question_retired'",
                   (long long)second_quiz);
    check(sqlite_integer(main_database_path, abandoned_state_sql) == 1,
          "abandon records retirement of each unresolved question");
    result = expect_success(root, database, "quiz.list",
                            COMMAND("quiz", "list", "--status", "abandoned", "--scope", "topic"));
    expect_json_integer(result.output, "$.count", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "report.active", COMMAND("report", "active"));
    expect_json_integer(result.output, "$.count", 0);
    run_result_destroy(&result);

    result = expect_success(root, database, "stats", COMMAND("stats"));
    expect_json_integer(result.output, "$.counts.quiz_sessions", 2);
    expect_json_integer(result.output, "$.counts.responses", 5);
    expect_json_integer(result.output, "$.counts.learning_records", 2);
    run_result_destroy(&result);
    result = expect_success(root, database, "db.backup", COMMAND("db", "backup", "backup.db"));
    check(json_integer(result.output, "$.bytes") > 0, "database backup is non-empty");
    run_result_destroy(&result);
    result = expect_failure(root, "restored.db", "usage", COMMAND("db", "restore", "backup.db"));
    run_result_destroy(&result);
    result = expect_success(root, "restored.db", "db.restore",
                            COMMAND("db", "restore", "backup.db", "--yes"));
    run_result_destroy(&result);
    result = expect_success(root, "restored.db", "db.doctor", COMMAND("db", "doctor"));
    expect_json_integer(result.output, "$.healthy", 1);
    run_result_destroy(&result);

    check(
        sqlite_execute(main_database_path,
                       "INSERT INTO source_units(corpus_slug,unit_key,unit_type,title,source_path,"
                       "position,start_page,end_page,start_line,end_line,include_in_quizzes,"
                       "included_reason,content_sha256,metadata_json) SELECT 'other-corpus',"
                       "unit_key,unit_type,title,'other-chapter.md',position,start_page,end_page,"
                       "start_line,end_line,0,included_reason,content_sha256,metadata_json FROM "
                       "source_units ORDER BY id LIMIT 1"),
        "ambiguous unit fixture is inserted");
    result = expect_failure(root, database, "conflict",
                            COMMAND("bank", "search", "--unit", "chapter-1"));
    check(json_array_length(result.error, "$.error.details.alternatives") == 2,
          "ambiguous unit errors enumerate canonical alternatives");
    expect_json_text(result.error, "$.error.details.alternatives[0].reference",
                     "example-book/chapter-1");
    run_result_destroy(&result);
    check(sqlite_execute(main_database_path,
                         "DELETE FROM source_units WHERE corpus_slug='other-corpus'"),
          "ambiguous unit fixture is removed");

    char *backup_path = path_join(root, "backup.db");
    char *trigger_tamper_path = path_join(root, "tamper-trigger.db");
    char *view_tamper_path = path_join(root, "tamper-view.db");
    check(backup_path != NULL && trigger_tamper_path != NULL && view_tamper_path != NULL &&
              copy_file(backup_path, trigger_tamper_path) &&
              copy_file(backup_path, view_tamper_path),
          "schema tamper databases are copied from a healthy backup");
    check(sqlite_execute(trigger_tamper_path, "DROP TRIGGER terminal_question_delete"),
          "required trigger is removed from tamper fixture");
    result = expect_failure(root, "tamper-trigger.db", "validation", COMMAND("db", "doctor"));
    expect_json_integer(result.error, "$.error.details.schema_fingerprint", 0);
    run_result_destroy(&result);
    check(
        sqlite_execute(view_tamper_path,
                       "DROP VIEW quiz_progress;CREATE VIEW quiz_progress AS SELECT 1 AS quiz_id"),
        "required view is altered in tamper fixture");
    result = expect_failure(root, "tamper-view.db", "validation", COMMAND("db", "doctor"));
    expect_json_integer(result.error, "$.error.details.schema_fingerprint", 0);
    run_result_destroy(&result);

    result = expect_success(root, database, "corpus.status", COMMAND("corpus", "status"));
    expect_json_integer(result.output, "$.in_sync", 1);
    run_result_destroy(&result);
    result = expect_success(root, database, "db.status", COMMAND("db", "status"));
    expect_json_integer(result.output, "$.counts.quiz_sessions", 2);
    run_result_destroy(&result);

    free(all_exports);
    free(exported);
    free(roundtrip_export);
    free(main_database_path);
    free(backup_path);
    free(trigger_tamper_path);
    free(view_tamper_path);
    return failures == 0;
}

int main(void) {
    if (sqlite3_open(":memory:", &json_database) != SQLITE_OK) {
        fputs("FAIL: cannot open SQLite JSON assertion database\n", stderr);
        return EXIT_FAILURE;
    }
    check(remove_tree(LBDB_TEST_WORK_DIR), "old integration fixtures are removed");
    check(make_directory(LBDB_TEST_WORK_DIR), "integration fixture directory is created");
    char *root = path_join(LBDB_TEST_WORK_DIR, "main");
    char *roundtrip_root = path_join(LBDB_TEST_WORK_DIR, "roundtrip");
    char *format_root = path_join(LBDB_TEST_WORK_DIR, "formats");
    check(root != NULL && roundtrip_root != NULL && format_root != NULL,
          "fixture root paths allocate");
    if (root != NULL && roundtrip_root != NULL && format_root != NULL) {
        check(setup_root(root), "main integration root is prepared");
        check(setup_root(roundtrip_root), "round-trip integration root is prepared");
        check(setup_root(format_root), "exchange-format integration root is prepared");
        if (failures == 0) {
            (void)test_strict_exchange_formats(format_root);
        }
        if (failures == 0) {
            (void)test_database_and_bank(root, roundtrip_root);
        }
    }
    free(root);
    free(roundtrip_root);
    free(format_root);
    sqlite3_close(json_database);
    if (failures != 0) {
        fprintf(stderr, "%d integration assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("integration_cli: all assertions passed");
    return EXIT_SUCCESS;
}
