#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const question_types[] = {
    "application", "code_reading",  "code_writing", "debugging",
    "example",     "misconception", "recall",       "relationship",
};
static const char *const response_formats[] = {"code_response", "free_response", "multiple_choice"};
static const char *const importance_values[] = {"core", "important", "supporting"};

typedef struct LbdbSectionRange {
    int64_t id;
    char *key;
    char *title;
    int64_t start_page;
    int64_t end_page;
    int64_t start_line;
    int64_t end_line;
    bool is_summary;
} LbdbSectionRange;

typedef struct LbdbSectionRanges {
    LbdbSectionRange *items;
    size_t count;
    size_t capacity;
} LbdbSectionRanges;

typedef struct LbdbQuestionData {
    char *question_type;
    char *response_format;
    char *prompt;
    char *options_json;
    char *expected_answer;
    char *criteria_json;
    char *answer_justification;
    char *source_section;
    char *source_pages;
    int64_t source_line_start;
    int64_t source_line_end;
    bool active;
    int64_t revision;
} LbdbQuestionData;

typedef struct LbdbBankFile {
    char *path;
    char *json;
} LbdbBankFile;

typedef struct LbdbBankFiles {
    LbdbBankFile *items;
    size_t count;
} LbdbBankFiles;

static void section_ranges_destroy(LbdbSectionRanges *ranges) {
    for (size_t index = 0; index < ranges->count; ++index) {
        free(ranges->items[index].key);
        free(ranges->items[index].title);
    }
    free(ranges->items);
    *ranges = (LbdbSectionRanges){0};
}

static void question_data_destroy(LbdbQuestionData *question) {
    free(question->question_type);
    free(question->response_format);
    free(question->prompt);
    free(question->options_json);
    free(question->expected_answer);
    free(question->criteria_json);
    free(question->answer_justification);
    free(question->source_section);
    free(question->source_pages);
    *question = (LbdbQuestionData){0};
}

static void bank_files_destroy(LbdbBankFiles *files) {
    for (size_t index = 0; index < files->count; ++index) {
        free(files->items[index].path);
        free(files->items[index].json);
    }
    free(files->items);
    *files = (LbdbBankFiles){0};
}

static bool nonempty_text(const char *value) {
    if (value == NULL) {
        return false;
    }
    for (; *value != '\0'; ++value) {
        if (!isspace((unsigned char)*value)) {
            return true;
        }
    }
    return false;
}

static LbdbError database_failure(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                                  const char *operation) {
    return error == LBDB_OK ? LBDB_OK : lbdb_app_database_error(app, database, error, operation);
}

static LbdbError step_statement(LbdbApp *app, LbdbDatabase *database, LbdbStatement *statement,
                                bool *has_row, const char *operation) {
    return database_failure(app, database, lbdb_statement_step(statement, has_row), operation);
}

static bool decimal_reference(const char *reference, int64_t *value) {
    char *end = NULL;
    long long parsed = 0;
    if (reference == NULL || reference[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoll(reference, &end, 10);
    if (errno != 0 || end == reference || *end != '\0' || parsed <= 0) {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static bool write_reference_alternative(LbdbJsonWriter *writer, int64_t id, const char *reference) {
    return lbdb_json_begin_object(writer) && lbdb_json_key(writer, "id") &&
           lbdb_json_int(writer, id) && lbdb_json_key(writer, "reference") &&
           lbdb_json_string(writer, reference) && lbdb_json_end_object(writer);
}

static LbdbError ambiguous_reference(LbdbApp *app, LbdbDatabase *database, LbdbStatement *statement,
                                     int64_t first_id, const char *first_reference,
                                     const char *kind, const char *reference) {
    LbdbJsonWriter *details = lbdb_json_writer_create(false);
    bool has_row = true;
    LbdbError error = LBDB_OK;
    if (details == NULL || !lbdb_json_begin_object(details) ||
        !lbdb_json_key(details, "alternatives") || !lbdb_json_begin_array(details) ||
        !write_reference_alternative(details, first_id, first_reference)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build reference alternatives");
    }
    while (error == LBDB_OK && has_row) {
        if (!write_reference_alternative(details, lbdb_statement_column_int64(statement, 0),
                                         lbdb_statement_column_text(statement, 1))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build reference alternatives");
            break;
        }
        error = step_statement(app, database, statement, &has_row,
                               "Cannot enumerate reference alternatives");
    }
    if (error == LBDB_OK && (!lbdb_json_end_array(details) || !lbdb_json_end_object(details))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build reference alternatives");
    }
    if (error == LBDB_OK) {
        error =
            lbdb_app_fail_details(app, LBDB_ERROR_CONFLICT, lbdb_json_data(details),
                                  "Ambiguous %s; use a canonical reference: %s", kind, reference);
    }
    lbdb_json_writer_destroy(details);
    return error;
}

LbdbError lbdb_resolve_unit_id(LbdbApp *app, LbdbDatabase *database, const char *reference,
                               int64_t *unit_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t numeric = 0;
    LbdbError error = LBDB_OK;
    if (!nonempty_text(reference)) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unit reference is required");
    }
    if (decimal_reference(reference, &numeric)) {
        error = lbdb_statement_prepare(
            database, "SELECT id,corpus_slug||'/'||unit_key FROM source_units WHERE id=?1",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, numeric);
        }
    } else {
        const char *separator = strrchr(reference, '/');
        if (separator != NULL) {
            const size_t slug_size = (size_t)(separator - reference);
            char *slug = malloc(slug_size + 1U);
            if (slug == NULL) {
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate unit reference");
            }
            memcpy(slug, reference, slug_size);
            slug[slug_size] = '\0';
            error = lbdb_statement_prepare(
                database,
                "SELECT id,corpus_slug||'/'||unit_key FROM source_units WHERE corpus_slug=?1 "
                "AND unit_key=?2",
                &statement);
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 1, slug);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 2, separator + 1);
            }
            free(slug);
        } else {
            error = lbdb_statement_prepare(database,
                                           "SELECT id,corpus_slug||'/'||unit_key FROM source_units "
                                           "WHERE unit_key=?1 "
                                           "ORDER BY corpus_slug",
                                           &statement);
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 1, reference);
            }
        }
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot resolve source unit");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown source unit: %s", reference);
    }
    if (error == LBDB_OK) {
        const int64_t first_id = lbdb_statement_column_int64(statement, 0);
        char *first_reference = lbdb_string_duplicate(lbdb_statement_column_text(statement, 1));
        bool second_row = false;
        if (first_reference == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate unit reference");
        } else {
            error =
                step_statement(app, database, statement, &second_row, "Cannot resolve source unit");
        }
        if (error == LBDB_OK && second_row) {
            error = ambiguous_reference(app, database, statement, first_id, first_reference,
                                        "unit key", reference);
        } else if (error == LBDB_OK) {
            *unit_id = first_id;
        }
        free(first_reference);
    }
    if (error != LBDB_OK && app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot resolve source unit");
    }
    lbdb_statement_destroy(statement);
    return error;
}

LbdbError lbdb_resolve_tag_id(LbdbApp *app, LbdbDatabase *database, const char *reference,
                              int64_t *tag_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t numeric = 0;
    LbdbError error = LBDB_OK;
    if (!nonempty_text(reference)) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Tag reference is required");
    }
    if (decimal_reference(reference, &numeric)) {
        error = lbdb_statement_prepare(database, "SELECT id,kind||':'||name FROM tags WHERE id=?1",
                                       &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, numeric);
        }
    } else {
        const char *separator = strchr(reference, ':');
        if (separator != NULL) {
            const size_t kind_size = (size_t)(separator - reference);
            char *kind = malloc(kind_size + 1U);
            if (kind == NULL) {
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate tag reference");
            }
            memcpy(kind, reference, kind_size);
            kind[kind_size] = '\0';
            error = lbdb_statement_prepare(
                database, "SELECT id,kind||':'||name FROM tags WHERE kind=?1 AND name=?2",
                &statement);
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 1, kind);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 2, separator + 1);
            }
            free(kind);
        } else {
            error = lbdb_statement_prepare(
                database,
                "SELECT DISTINCT t.id,t.kind||':'||t.name FROM tags t LEFT JOIN tag_aliases a ON "
                "a.tag_id=t.id WHERE t.name=?1 COLLATE NOCASE OR a.alias=?1 COLLATE NOCASE "
                "ORDER BY t.kind,t.name,t.id",
                &statement);
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 1, reference);
            }
        }
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot resolve tag");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown tag: %s", reference);
    }
    if (error == LBDB_OK) {
        const int64_t first_id = lbdb_statement_column_int64(statement, 0);
        char *first_reference = lbdb_string_duplicate(lbdb_statement_column_text(statement, 1));
        bool second_row = false;
        if (first_reference == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate tag reference");
        } else {
            error = step_statement(app, database, statement, &second_row, "Cannot resolve tag");
        }
        if (error == LBDB_OK && second_row) {
            error = ambiguous_reference(app, database, statement, first_id, first_reference,
                                        "tag name", reference);
        } else if (error == LBDB_OK) {
            *tag_id = first_id;
        }
        free(first_reference);
    }
    if (error != LBDB_OK && app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot resolve tag");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError push_section_range(LbdbApp *app, LbdbSectionRanges *ranges,
                                    const LbdbSectionRange *range) {
    LbdbSectionRange *replacement = NULL;
    size_t capacity = 0U;
    if (ranges->count == ranges->capacity) {
        capacity = ranges->capacity == 0U ? 4U : ranges->capacity * 2U;
        if (capacity < ranges->capacity || capacity > SIZE_MAX / sizeof(*replacement)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Source range has too many sections");
        }
        replacement = realloc(ranges->items, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source sections");
        }
        ranges->items = replacement;
        ranges->capacity = capacity;
    }
    ranges->items[ranges->count++] = *range;
    return LBDB_OK;
}

static LbdbError sections_for_range(LbdbApp *app, LbdbDatabase *database, int64_t unit_id,
                                    int64_t line_start, int64_t line_end,
                                    LbdbSectionRanges *ranges) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = LBDB_OK;
    if (line_start <= 0 || line_end < line_start) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Source line range is invalid");
    }
    error = lbdb_statement_prepare(
        database,
        "SELECT id,section_key,title,start_page,end_page,start_line,end_line,is_summary "
        "FROM source_sections WHERE unit_id=?1 AND start_line<=?2 AND end_line>=?3 "
        "ORDER BY position",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, unit_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, line_end);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 3, line_start);
    }
    while (error == LBDB_OK) {
        LbdbSectionRange range = {0};
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        range.id = lbdb_statement_column_int64(statement, 0);
        range.key = lbdb_string_duplicate(lbdb_statement_column_text(statement, 1));
        range.title = lbdb_string_duplicate(lbdb_statement_column_text(statement, 2));
        range.start_page = lbdb_statement_column_int64(statement, 3);
        range.end_page = lbdb_statement_column_int64(statement, 4);
        range.start_line = lbdb_statement_column_int64(statement, 5);
        range.end_line = lbdb_statement_column_int64(statement, 6);
        range.is_summary = lbdb_statement_column_int64(statement, 7) != 0;
        if (range.key == NULL || range.title == NULL) {
            free(range.key);
            free(range.title);
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source section");
        } else {
            error = push_section_range(app, ranges, &range);
            if (error != LBDB_OK) {
                free(range.key);
                free(range.title);
            }
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot resolve source range");
    }
    lbdb_statement_destroy(statement);
    if (error != LBDB_OK) {
        return error;
    }
    if (ranges->count == 0U || ranges->items[0].start_line > line_start ||
        ranges->items[ranges->count - 1U].end_line < line_end) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                             "Source range is not fully covered by indexed sections");
    }
    for (size_t index = 0; index < ranges->count; ++index) {
        if (ranges->items[index].is_summary) {
            return lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                 "Summary sections cannot support concepts or questions");
        }
        if (index > 0U &&
            ranges->items[index].start_line > ranges->items[index - 1U].end_line + 1) {
            return lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                 "Source range crosses an unindexed line gap");
        }
    }
    return LBDB_OK;
}

static LbdbError validate_string_array(LbdbApp *app, LbdbDatabase *database, const char *json,
                                       const char *field, size_t minimum, size_t maximum) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_json_document_type(app, database, json, "array");
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT count(*),coalesce(sum(type='text' AND length(trim(value))>0),0) "
            "FROM json_each(?1)",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, json);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot validate JSON array");
    }
    if (error == LBDB_OK) {
        const int64_t count = lbdb_statement_column_int64(statement, 0);
        const int64_t valid = lbdb_statement_column_int64(statement, 1);
        if (!has_row || count < (int64_t)minimum || count > (int64_t)maximum || count != valid) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "%s must contain %zu-%zu non-empty strings", field, minimum, maximum);
        }
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError expected_answer_matches(LbdbApp *app, LbdbDatabase *database,
                                         const char *options_json, const char *expected_answer) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database, "SELECT count(*) FROM json_each(?1) WHERE type='text' AND value=?2", &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, options_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, expected_answer);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row,
                               "Cannot validate multiple-choice answer");
    }
    if (error == LBDB_OK && (!has_row || lbdb_statement_column_int64(statement, 0) != 1)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Multiple-choice expected answer must match exactly one option");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError parse_question_data(LbdbApp *app, LbdbDatabase *database, const char *json,
                                     LbdbQuestionData *question) {
    bool present = false;
    int64_t integer = 0;
    LbdbError error = lbdb_json_document_type(app, database, json, "object");
#define GET_TEXT(path, target)                                                                     \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_json_get_text(app, database, json, (path), true, &(target));              \
        }                                                                                          \
    } while (false)
    GET_TEXT("$.question_type", question->question_type);
    GET_TEXT("$.response_format", question->response_format);
    GET_TEXT("$.prompt", question->prompt);
    GET_TEXT("$.expected_answer", question->expected_answer);
    GET_TEXT("$.answer_justification", question->answer_justification);
    GET_TEXT("$.source_section", question->source_section);
    GET_TEXT("$.source_pages", question->source_pages);
#undef GET_TEXT
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, json, "$.options", false, &question->options_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, json, "$.grading_criteria", true,
                                  &question->criteria_json);
    }
    if (error == LBDB_OK) {
        error =
            lbdb_json_get_int(app, database, json, "$.source_line_start", true, &integer, &present);
        question->source_line_start = integer;
    }
    if (error == LBDB_OK) {
        error =
            lbdb_json_get_int(app, database, json, "$.source_line_end", true, &integer, &present);
        question->source_line_end = integer;
    }
    question->active = true;
    if (error == LBDB_OK) {
        bool active = true;
        error = lbdb_json_get_bool(app, database, json, "$.active", false, &active, &present);
        if (present) {
            question->active = active;
        }
    }
    question->revision = 1;
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.revision", false, &integer, &present);
        if (present) {
            question->revision = integer;
        }
    }
    if (error == LBDB_OK &&
        (!nonempty_text(question->question_type) || !nonempty_text(question->response_format) ||
         !nonempty_text(question->prompt) || !nonempty_text(question->expected_answer) ||
         !nonempty_text(question->answer_justification) ||
         !nonempty_text(question->source_section) || !nonempty_text(question->source_pages))) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                          "Question text, answer, justification, and source fields are required");
    }
    if (error == LBDB_OK &&
        !lbdb_string_in_set(question->question_type, question_types,
                            sizeof(question_types) / sizeof(question_types[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unknown question type: %s",
                              question->question_type);
    }
    if (error == LBDB_OK &&
        !lbdb_string_in_set(question->response_format, response_formats,
                            sizeof(response_formats) / sizeof(response_formats[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unknown response format: %s",
                              question->response_format);
    }
    if (error == LBDB_OK &&
        (question->source_line_start <= 0 ||
         question->source_line_end < question->source_line_start || question->revision <= 0)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Question source lines and revision must be positive and ordered");
    }
    if (error == LBDB_OK) {
        error = validate_string_array(app, database, question->criteria_json, "grading_criteria",
                                      1U, (size_t)INT64_MAX);
    }
    if (error == LBDB_OK && strcmp(question->response_format, "multiple_choice") == 0) {
        if (question->options_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Multiple-choice questions require options");
        } else {
            error = validate_string_array(app, database, question->options_json, "options", 3U, 5U);
        }
        if (error == LBDB_OK) {
            error = expected_answer_matches(app, database, question->options_json,
                                            question->expected_answer);
        }
    } else if (error == LBDB_OK && question->options_json != NULL) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Only multiple-choice questions may contain options");
    }
    if (error != LBDB_OK) {
        question_data_destroy(question);
    }
    return error;
}

static double checkpoint_for_range(int64_t unit_start, int64_t unit_end, int64_t source_end) {
    const int64_t total = unit_end - unit_start + 1;
    int64_t progress = source_end - unit_start + 1;
    if (progress < 1) {
        progress = 1;
    }
    int64_t quarter = (progress * 4 + total - 1) / total;
    if (quarter < 1) {
        quarter = 1;
    } else if (quarter > 4) {
        quarter = 4;
    }
    return (double)quarter / 4.0;
}

static LbdbError get_or_create_tag(LbdbApp *app, LbdbDatabase *database, const char *kind,
                                   const char *name, const char *description, int64_t *tag_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "INSERT INTO tags(kind,name,description,metadata_json) VALUES(?1,?2,?3,'{}') "
        "ON CONFLICT(kind,name) DO NOTHING",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, kind);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, name);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 3, description);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, NULL, "Cannot create tag");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database, "SELECT id FROM tags WHERE kind=?1 AND name=?2",
                                       &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, kind);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, name);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot resolve created tag");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Created tag cannot be resolved");
    }
    if (error == LBDB_OK) {
        *tag_id = lbdb_statement_column_int64(statement, 0);
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError insert_question_tags(LbdbApp *app, LbdbDatabase *database, int64_t question_id,
                                      const char *tags_json) {
    LbdbStatement *iterator = NULL;
    LbdbStatement *link = NULL;
    bool has_row = false;
    bool topic = false;
    bool theme = false;
    bool mode = false;
    int64_t count = 0;
    LbdbError error = lbdb_json_document_type(app, database, tags_json, "array");
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT json_extract(value,'$.kind'),json_extract(value,'$.name'),"
            "coalesce(json_extract(value,'$.description'),json_extract(value,'$.name')),type "
            "FROM json_each(?1) ORDER BY CAST(key AS INTEGER)",
            &iterator);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(iterator, 1, tags_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "INSERT OR IGNORE INTO question_tags(question_id,tag_id) VALUES(?1,?2)",
            &link);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(iterator, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const char *kind = lbdb_statement_column_text(iterator, 0);
        const char *name = lbdb_statement_column_text(iterator, 1);
        const char *description = lbdb_statement_column_text(iterator, 2);
        const char *type = lbdb_statement_column_text(iterator, 3);
        if (type == NULL || strcmp(type, "object") != 0 || !nonempty_text(kind) ||
            !nonempty_text(name) || !nonempty_text(description)) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Tags must be objects with non-empty kind, name, and description");
            break;
        }
        int64_t tag_id = 0;
        error = get_or_create_tag(app, database, kind, name, description, &tag_id);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(link, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(link, 2, tag_id);
        }
        if (error == LBDB_OK) {
            error = step_statement(app, database, link, NULL, "Cannot link question tag");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_reset(link);
        }
        if (strcasecmp(kind, "topic") == 0) {
            topic = true;
        } else if (strcasecmp(kind, "theme") == 0) {
            theme = true;
        } else if (strcasecmp(kind, "mode") == 0) {
            mode = true;
        }
        count += 1;
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot parse question tags");
    }
    if (error == LBDB_OK && (count == 0 || !topic || !theme || !mode)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Every question requires topic, theme, and mode tags");
    }
    lbdb_statement_destroy(iterator);
    lbdb_statement_destroy(link);
    return error;
}

static LbdbError insert_source_links(LbdbApp *app, LbdbDatabase *database, const char *owner,
                                     int64_t owner_id, int64_t unit_id, const char *owner_json,
                                     int64_t source_start, int64_t source_end,
                                     const char *default_justification) {
    LbdbSectionRanges ranges = {0};
    char *sources_json = NULL;
    LbdbStatement *insert = NULL;
    LbdbStatement *iterator = NULL;
    bool has_row = false;
    LbdbError error = sections_for_range(app, database, unit_id, source_start, source_end, &ranges);
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, owner_json, "$.sources", false, &sources_json);
    }
    const bool explicit_sources = sources_json != NULL;
    if (error == LBDB_OK && explicit_sources) {
        error = lbdb_json_document_type(app, database, sources_json, "array");
        if (error == LBDB_OK) {
            size_t source_count = 0U;
            error = lbdb_json_array_size(app, database, owner_json, "$.sources", &source_count);
            if (error == LBDB_OK && source_count == 0U) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "sources must be a non-empty array");
            }
        }
    }
    const char *sql =
        strcmp(owner, "concept") == 0
            ? "INSERT INTO concept_sources(concept_id,position,section_id,source_pages,"
              "source_line_start,source_line_end,role,justification) "
              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
            : "INSERT INTO question_sources(question_id,position,section_id,"
              "source_pages,source_line_start,source_line_end,role,justification) "
              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)";
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database, sql, &insert);
    }
    if (error == LBDB_OK && explicit_sources) {
        error = lbdb_statement_prepare(
            database, "SELECT CAST(key AS INTEGER)+1,value,type FROM json_each(?1)", &iterator);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(iterator, 1, sources_json);
        }
    }
    size_t automatic_index = 0U;
    for (;;) {
        int64_t position = 0;
        int64_t section_id = 0;
        int64_t line_start = 0;
        int64_t line_end = 0;
        char *pages = NULL;
        char *role = NULL;
        char *justification = NULL;
        char *item_json = NULL;
        if (error != LBDB_OK) {
            break;
        }
        if (explicit_sources) {
            error = lbdb_statement_step(iterator, &has_row);
            if (error != LBDB_OK || !has_row) {
                break;
            }
            if (strcmp(lbdb_statement_column_text(iterator, 2), "object") != 0) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Each source link must be an object");
                break;
            }
            position = lbdb_statement_column_int64(iterator, 0);
            item_json = lbdb_string_duplicate(lbdb_statement_column_text(iterator, 1));
            char *section_key = NULL;
            bool present = false;
            if (item_json == NULL) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source link");
            }
            if (error == LBDB_OK) {
                error = lbdb_json_get_text(app, database, item_json, "$.section_key", true,
                                           &section_key);
            }
            if (error == LBDB_OK) {
                error =
                    lbdb_json_get_text(app, database, item_json, "$.source_pages", true, &pages);
            }
            if (error == LBDB_OK) {
                error = lbdb_json_get_int(app, database, item_json, "$.source_line_start", true,
                                          &line_start, &present);
            }
            if (error == LBDB_OK) {
                error = lbdb_json_get_int(app, database, item_json, "$.source_line_end", true,
                                          &line_end, &present);
            }
            if (error == LBDB_OK) {
                error = lbdb_json_get_text(app, database, item_json, "$.role", true, &role);
            }
            if (error == LBDB_OK) {
                error = lbdb_json_get_text(app, database, item_json, "$.justification", true,
                                           &justification);
            }
            LbdbStatement *section = NULL;
            bool section_row = false;
            if (error == LBDB_OK) {
                error = lbdb_statement_prepare(
                    database,
                    "SELECT id,start_line,end_line,is_summary FROM source_sections "
                    "WHERE unit_id=?1 AND section_key=?2",
                    &section);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_int64(section, 1, unit_id);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(section, 2, section_key);
            }
            if (error == LBDB_OK) {
                error = step_statement(app, database, section, &section_row,
                                       "Cannot resolve source section");
            }
            if (error == LBDB_OK && (!section_row || lbdb_statement_column_int64(section, 3) != 0 ||
                                     line_start < lbdb_statement_column_int64(section, 1) ||
                                     line_end > lbdb_statement_column_int64(section, 2) ||
                                     line_start <= 0 || line_end < line_start)) {
                error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                      "Source link is outside a body section: %s", section_key);
            }
            if (error == LBDB_OK) {
                section_id = lbdb_statement_column_int64(section, 0);
            }
            lbdb_statement_destroy(section);
            free(section_key);
        } else {
            if (automatic_index >= ranges.count) {
                break;
            }
            const LbdbSectionRange *range = &ranges.items[automatic_index];
            position = (int64_t)automatic_index + 1;
            section_id = range->id;
            line_start = source_start > range->start_line ? source_start : range->start_line;
            line_end = source_end < range->end_line ? source_end : range->end_line;
            pages = range->start_page == range->end_page
                        ? lbdb_string_format("%04lld", (long long)range->start_page)
                        : lbdb_string_format("%04lld-%04lld", (long long)range->start_page,
                                             (long long)range->end_page);
            role = lbdb_string_duplicate(automatic_index == 0U ? "primary" : "supporting");
            justification = lbdb_string_duplicate(default_justification);
            automatic_index += 1U;
            if (pages == NULL || role == NULL || justification == NULL) {
                (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY,
                                    "Unable to allocate automatic source link");
                error = LBDB_ERROR_MEMORY;
            }
        }
        if (error == LBDB_OK && (strcmp(role, "primary") != 0 && strcmp(role, "supporting") != 0)) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Source role must be primary or supporting");
        }
        if (error == LBDB_OK && (!nonempty_text(pages) || !nonempty_text(justification))) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Source pages and justification are required");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 1, owner_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 2, position);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 3, section_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(insert, 4, pages);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 5, line_start);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 6, line_end);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(insert, 7, role);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(insert, 8, justification);
        }
        if (error == LBDB_OK) {
            error = step_statement(app, database, insert, NULL, "Cannot insert source link");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_reset(insert);
        }
        free(item_json);
        free(pages);
        free(role);
        free(justification);
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot parse source links");
    }
    lbdb_statement_destroy(iterator);
    lbdb_statement_destroy(insert);
    free(sources_json);
    section_ranges_destroy(&ranges);
    return error;
}

static LbdbError validate_flat_provenance(LbdbApp *app, LbdbDatabase *database, const char *owner,
                                          int64_t owner_id, const char *source_pages,
                                          int64_t source_start, int64_t source_end,
                                          int64_t primary_section_id, const char *source_section) {
    const bool concept = strcmp(owner, "concept") == 0;
    const char *sql =
        concept ? "SELECT count(*),min(l.source_line_start),max(l.source_line_end),"
                  "coalesce(sum(instr(?2,l.source_pages)=0 AND instr(l.source_pages,?2)=0),0),"
                  "coalesce(sum(l.role='primary'),0),"
                  "coalesce(min(CASE WHEN l.role='primary' THEN l.section_id END),0),NULL "
                  "FROM concept_sources l JOIN source_sections s ON s.id=l.section_id "
                  "WHERE l.concept_id=?1"
                : "SELECT count(*),min(l.source_line_start),max(l.source_line_end),"
                  "coalesce(sum(instr(?2,l.source_pages)=0 AND instr(l.source_pages,?2)=0),0),"
                  "coalesce(sum(l.role='primary'),0),"
                  "coalesce(min(CASE WHEN l.role='primary' THEN l.section_id END),0),"
                  "min(CASE WHEN l.role='primary' THEN s.title END) FROM question_sources l "
                  "JOIN source_sections s ON s.id=l.section_id WHERE l.question_id=?1";
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(database, sql, &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, owner_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, source_pages);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row,
                               "Cannot validate flat source provenance");
    }
    const char *canonical_section =
        error == LBDB_OK && has_row ? lbdb_statement_column_text(statement, 6) : NULL;
    const bool matches = error == LBDB_OK && has_row &&
                         lbdb_statement_column_int64(statement, 0) > 0 &&
                         lbdb_statement_column_int64(statement, 1) == source_start &&
                         lbdb_statement_column_int64(statement, 2) == source_end &&
                         lbdb_statement_column_int64(statement, 3) == 0 &&
                         lbdb_statement_column_int64(statement, 4) == 1 &&
                         lbdb_statement_column_int64(statement, 5) == primary_section_id &&
                         (concept || (canonical_section != NULL && source_section != NULL &&
                                      strcmp(canonical_section, source_section) == 0));
    if (error == LBDB_OK && !matches) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "%s flat provenance must match its canonical source links", owner);
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError resolve_bank_unit(LbdbApp *app, LbdbDatabase *database, const char *json,
                                   int64_t *unit_id, int64_t *unit_start, int64_t *unit_end,
                                   char **book_slug, char **unit_key) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_json_get_text(app, database, json, "$.book_slug", true, book_slug);
    if (error == LBDB_OK) {
        error = lbdb_json_get_text(app, database, json, "$.unit_key", true, unit_key);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,start_line,end_line FROM source_units WHERE corpus_slug=?1 AND unit_key=?2",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, *book_slug);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, *unit_key);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot resolve bank unit");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown source unit: %s/%s", *book_slug,
                              *unit_key);
    }
    if (error == LBDB_OK) {
        *unit_id = lbdb_statement_column_int64(statement, 0);
        *unit_start = lbdb_statement_column_int64(statement, 1);
        *unit_end = lbdb_statement_column_int64(statement, 2);
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError concept_coverage(LbdbApp *app, LbdbDatabase *database, const char *json,
                                  int64_t *theory, int64_t *application, int64_t *code,
                                  int64_t *semantics) {
    char *coverage = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_json_get_raw(app, database, json, "$.coverage", true, &coverage);
    if (error == LBDB_OK) {
        error = lbdb_json_document_type(app, database, coverage, "array");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT count(*),coalesce(sum(type='text' AND value IN('theory','application',"
            "'code','semantics')),0),coalesce(max(value='theory'),0),"
            "coalesce(max(value='application'),0),coalesce(max(value='code'),0),"
            "coalesce(max(value='semantics'),0) FROM json_each(?1)",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, coverage);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot validate coverage");
    }
    if (error == LBDB_OK &&
        (!has_row || lbdb_statement_column_int64(statement, 0) == 0 ||
         lbdb_statement_column_int64(statement, 0) != lbdb_statement_column_int64(statement, 1))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Concept coverage must contain only theory, application, code, or "
                              "semantics");
    }
    if (error == LBDB_OK) {
        *theory = lbdb_statement_column_int64(statement, 2);
        *application = lbdb_statement_column_int64(statement, 3);
        *code = lbdb_statement_column_int64(statement, 4);
        *semantics = lbdb_statement_column_int64(statement, 5);
    }
    lbdb_statement_destroy(statement);
    free(coverage);
    return error;
}

static LbdbError import_question(LbdbApp *app, LbdbDatabase *database, const char *question_json,
                                 const char *fallback_tags, int64_t unit_id, int64_t concept_id,
                                 int64_t unit_start, int64_t unit_end, int64_t position,
                                 int64_t *question_id) {
    LbdbQuestionData question = {0};
    LbdbSectionRanges ranges = {0};
    LbdbStatement *statement = NULL;
    char *tags_json = NULL;
    LbdbError error = parse_question_data(app, database, question_json, &question);
    if (error == LBDB_OK) {
        error = sections_for_range(app, database, unit_id, question.source_line_start,
                                   question.source_line_end, &ranges);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO question_bank(unit_id,concept_id,position,earliest_checkpoint,"
            "question_type,response_format,prompt,options_json,expected_answer,"
            "grading_criteria_json,answer_justification,source_section,source_pages,"
            "source_line_start,source_line_end,body_verified,active,revision,metadata_json) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,1,?16,?17,'{}')",
            &statement);
    }
    int index = 1;
#define QB_TEXT(value)                                                                             \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(statement, index++, (value));                         \
        }                                                                                          \
    } while (false)
#define QB_INT(value)                                                                              \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(statement, index++, (value));                        \
        }                                                                                          \
    } while (false)
    QB_INT(unit_id);
    QB_INT(concept_id);
    QB_INT(position);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_double(
            statement, index++,
            checkpoint_for_range(unit_start, unit_end, question.source_line_end));
    }
    QB_TEXT(question.question_type);
    QB_TEXT(question.response_format);
    QB_TEXT(question.prompt);
    QB_TEXT(question.options_json);
    QB_TEXT(question.expected_answer);
    QB_TEXT(question.criteria_json);
    QB_TEXT(question.answer_justification);
    QB_TEXT(question.source_section);
    QB_TEXT(question.source_pages);
    QB_INT(question.source_line_start);
    QB_INT(question.source_line_end);
    QB_INT(question.active ? 1 : 0);
    QB_INT(question.revision);
#undef QB_TEXT
#undef QB_INT
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, NULL, "Cannot insert bank question");
    }
    if (error == LBDB_OK) {
        *question_id = lbdb_statement_last_insert_id(statement);
        error = insert_source_links(app, database, "question", *question_id, unit_id, question_json,
                                    question.source_line_start, question.source_line_end,
                                    question.answer_justification);
    }
    if (error == LBDB_OK) {
        error = validate_flat_provenance(
            app, database, "question", *question_id, question.source_pages,
            question.source_line_start, question.source_line_end,
            ranges.count > 0U ? ranges.items[0].id : 0, question.source_section);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, question_json, "$.tags", false, &tags_json);
    }
    if (error == LBDB_OK && tags_json == NULL && fallback_tags != NULL) {
        tags_json = lbdb_string_duplicate(fallback_tags);
        if (tags_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate inherited tags");
        }
    }
    if (error == LBDB_OK && tags_json == NULL) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Question tags are required");
    }
    if (error == LBDB_OK) {
        error = insert_question_tags(app, database, *question_id, tags_json);
    }
    free(tags_json);
    lbdb_statement_destroy(statement);
    section_ranges_destroy(&ranges);
    question_data_destroy(&question);
    return error;
}

static LbdbError import_bank_object(LbdbApp *app, LbdbDatabase *database, const char *json,
                                    const char *source_name, int64_t *concept_count,
                                    int64_t *question_count, int64_t *imported_unit_id,
                                    char **unit_label) {
    char *book_slug = NULL;
    char *unit_key = NULL;
    char *concepts_json = NULL;
    LbdbStatement *existing = NULL;
    LbdbStatement *concept_iterator = NULL;
    bool has_row = false;
    int64_t unit_id = 0;
    int64_t unit_start = 0;
    int64_t unit_end = 0;
    bool version_present = false;
    int64_t format_version = 0;
    LbdbError error = lbdb_json_document_type(app, database, json, "object");
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.format_version", true, &format_version,
                                  &version_present);
    }
    if (error == LBDB_OK && format_version != 1) {
        error = lbdb_app_fail(app, LBDB_ERROR_UNSUPPORTED, "Unsupported bank format version: %lld",
                              (long long)format_version);
    }
    if (error == LBDB_OK) {
        error = resolve_bank_unit(app, database, json, &unit_id, &unit_start, &unit_end, &book_slug,
                                  &unit_key);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database, "SELECT count(*) FROM concepts WHERE unit_id=?1",
                                       &existing);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(existing, 1, unit_id);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, existing, &has_row, "Cannot inspect existing bank");
    }
    if (error == LBDB_OK && has_row && lbdb_statement_column_int64(existing, 0) != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Bank already exists for %s/%s", book_slug,
                              unit_key);
    }
    lbdb_statement_destroy(existing);
    existing = NULL;
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, json, "$.concepts", true, &concepts_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_document_type(app, database, concepts_json, "array");
    }
    if (error == LBDB_OK) {
        error =
            lbdb_statement_prepare(database,
                                   "SELECT CAST(key AS INTEGER)+1,value,type FROM json_each(?1) "
                                   "ORDER BY CAST(key AS INTEGER)",
                                   &concept_iterator);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(concept_iterator, 1, concepts_json);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(concept_iterator, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const int64_t concept_position = lbdb_statement_column_int64(concept_iterator, 0);
        const char *concept_type = lbdb_statement_column_text(concept_iterator, 2);
        char *concept_json = NULL;
        char *name = NULL;
        char *description = NULL;
        char *importance = NULL;
        char *source_pages = NULL;
        char *justification = NULL;
        char *concept_tags = NULL;
        char *questions_json = NULL;
        int64_t line_start = 0;
        int64_t line_end = 0;
        int64_t theory = 0;
        int64_t application = 0;
        int64_t code = 0;
        int64_t semantics = 0;
        bool present = false;
        LbdbSectionRanges ranges = {0};
        LbdbStatement *insert_concept = NULL;
        LbdbStatement *question_iterator = NULL;
        if (concept_type == NULL || strcmp(concept_type, "object") != 0) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Each concept must be an object");
            break;
        }
        concept_json = lbdb_string_duplicate(lbdb_statement_column_text(concept_iterator, 1));
        if (concept_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate concept JSON");
        }
#define CONCEPT_TEXT(path, target)                                                                 \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_json_get_text(app, database, concept_json, (path), true, &(target));      \
        }                                                                                          \
    } while (false)
        CONCEPT_TEXT("$.name", name);
        CONCEPT_TEXT("$.description", description);
        CONCEPT_TEXT("$.importance", importance);
        CONCEPT_TEXT("$.source_pages", source_pages);
        CONCEPT_TEXT("$.justification", justification);
#undef CONCEPT_TEXT
        if (error == LBDB_OK) {
            error = lbdb_json_get_int(app, database, concept_json, "$.source_line_start", true,
                                      &line_start, &present);
        }
        if (error == LBDB_OK) {
            error = lbdb_json_get_int(app, database, concept_json, "$.source_line_end", true,
                                      &line_end, &present);
        }
        if (error == LBDB_OK &&
            !lbdb_string_in_set(importance, importance_values,
                                sizeof(importance_values) / sizeof(importance_values[0]))) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Concept importance must be core, important, or supporting");
        }
        if (error == LBDB_OK) {
            error = concept_coverage(app, database, concept_json, &theory, &application, &code,
                                     &semantics);
        }
        if (error == LBDB_OK) {
            error = sections_for_range(app, database, unit_id, line_start, line_end, &ranges);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database,
                "INSERT INTO concepts(unit_id,primary_section_id,position,name,description,"
                "importance,theory_required,application_required,code_required,"
                "semantics_required,source_pages,source_line_start,source_line_end,"
                "justification,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,"
                "?12,?13,?14,?15)",
                &insert_concept);
        }
        int bind = 1;
#define CONCEPT_BIND_TEXT(value)                                                                   \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(insert_concept, bind++, (value));                     \
        }                                                                                          \
    } while (false)
#define CONCEPT_BIND_INT(value)                                                                    \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(insert_concept, bind++, (value));                    \
        }                                                                                          \
    } while (false)
        CONCEPT_BIND_INT(unit_id);
        CONCEPT_BIND_INT(ranges.count > 0U ? ranges.items[0].id : 0);
        CONCEPT_BIND_INT(concept_position);
        CONCEPT_BIND_TEXT(name);
        CONCEPT_BIND_TEXT(description);
        CONCEPT_BIND_TEXT(importance);
        CONCEPT_BIND_INT(theory);
        CONCEPT_BIND_INT(application);
        CONCEPT_BIND_INT(code);
        CONCEPT_BIND_INT(semantics);
        CONCEPT_BIND_TEXT(source_pages);
        CONCEPT_BIND_INT(line_start);
        CONCEPT_BIND_INT(line_end);
        CONCEPT_BIND_TEXT(justification);
        CONCEPT_BIND_TEXT("{}");
#undef CONCEPT_BIND_TEXT
#undef CONCEPT_BIND_INT
        if (error == LBDB_OK) {
            error = step_statement(app, database, insert_concept, NULL, "Cannot insert concept");
        }
        const int64_t concept_id =
            error == LBDB_OK ? lbdb_statement_last_insert_id(insert_concept) : 0;
        if (error == LBDB_OK) {
            error = insert_source_links(app, database, "concept", concept_id, unit_id, concept_json,
                                        line_start, line_end, justification);
        }
        if (error == LBDB_OK) {
            error = validate_flat_provenance(app, database, "concept", concept_id, source_pages,
                                             line_start, line_end,
                                             ranges.count > 0U ? ranges.items[0].id : 0, NULL);
        }
        if (error == LBDB_OK) {
            error = lbdb_json_get_raw(app, database, concept_json, "$.tags", false, &concept_tags);
        }
        if (error == LBDB_OK && concept_tags != NULL) {
            error = lbdb_json_document_type(app, database, concept_tags, "array");
        }
        if (error == LBDB_OK) {
            error = lbdb_json_get_raw(app, database, concept_json, "$.questions", true,
                                      &questions_json);
        }
        if (error == LBDB_OK) {
            error = lbdb_json_document_type(app, database, questions_json, "array");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(database,
                                           "SELECT value,type FROM json_each(?1) "
                                           "ORDER BY CAST(key AS INTEGER)",
                                           &question_iterator);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(question_iterator, 1, questions_json);
        }
        int64_t concept_questions = 0;
        while (error == LBDB_OK) {
            bool question_row = false;
            error = lbdb_statement_step(question_iterator, &question_row);
            if (error != LBDB_OK || !question_row) {
                break;
            }
            if (strcmp(lbdb_statement_column_text(question_iterator, 1), "object") != 0) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Each question must be an object");
                break;
            }
            const char *question_json = lbdb_statement_column_text(question_iterator, 0);
            int64_t question_id = 0;
            *question_count += 1;
            concept_questions += 1;
            error = import_question(app, database, question_json, concept_tags, unit_id, concept_id,
                                    unit_start, unit_end, *question_count, &question_id);
        }
        if (error == LBDB_OK && concept_questions == 0) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Every concept requires at least one question");
        }
        if (error == LBDB_OK) {
            *concept_count += 1;
        }
        lbdb_statement_destroy(question_iterator);
        lbdb_statement_destroy(insert_concept);
        section_ranges_destroy(&ranges);
        free(concept_json);
        free(name);
        free(description);
        free(importance);
        free(source_pages);
        free(justification);
        free(concept_tags);
        free(questions_json);
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot parse bank JSON");
    }
    if (error == LBDB_OK && *concept_count == 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Bank requires at least one concept");
    }
    if (error == LBDB_OK) {
        *imported_unit_id = unit_id;
        *unit_label = lbdb_string_format("%s/%s", book_slug, unit_key);
        if (*unit_label == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate unit label");
        }
    }
    (void)source_name;
    (void)version_present;
    lbdb_statement_destroy(concept_iterator);
    free(concepts_json);
    free(book_slug);
    free(unit_key);
    return error;
}

static LbdbError load_bank_files(LbdbApp *app, const LbdbStringVector *paths,
                                 LbdbBankFiles *files) {
    files->items = calloc(paths->count, sizeof(*files->items));
    if (files->items == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate bank files");
    }
    files->count = paths->count;
    for (size_t index = 0; index < paths->count; ++index) {
        size_t size = 0U;
        LbdbError error =
            lbdb_resolve_path(app, paths->items[index], true, false, &files->items[index].path);
        if (error == LBDB_OK) {
            error = lbdb_read_file(app, files->items[index].path, &files->items[index].json, &size);
        }
        (void)size;
        if (error != LBDB_OK) {
            return error;
        }
    }
    return LBDB_OK;
}

LbdbError lbdb_command_bank_import(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbStringVector paths = {0};
    LbdbBankFiles files = {0};
    LbdbDatabase *database = NULL;
    LbdbJsonWriter *imported = NULL;
    int64_t total_concepts = 0;
    int64_t total_questions = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_positionals(&args, &paths);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && paths.count == 0U) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "bank import requires one or more FILE paths");
    }
    if (error == LBDB_OK) {
        error = load_bank_files(app, &paths, &files);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    imported = lbdb_json_writer_create(app->pretty);
    if (error == LBDB_OK && (imported == NULL || !lbdb_json_begin_array(imported))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate import output");
    }
    for (size_t index = 0; error == LBDB_OK && index < files.count; ++index) {
        int64_t concepts = 0;
        int64_t questions = 0;
        int64_t unit_id = 0;
        char *unit_label = NULL;
        error = import_bank_object(app, database, files.items[index].json, files.items[index].path,
                                   &concepts, &questions, &unit_id, &unit_label);
        if (error == LBDB_OK &&
            (!lbdb_json_begin_object(imported) || !lbdb_json_key(imported, "unit_id") ||
             !lbdb_json_int(imported, unit_id) || !lbdb_json_key(imported, "unit") ||
             !lbdb_json_string(imported, unit_label) || !lbdb_json_key(imported, "concepts") ||
             !lbdb_json_int(imported, concepts) || !lbdb_json_key(imported, "questions") ||
             !lbdb_json_int(imported, questions) || !lbdb_json_end_object(imported))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build import output");
        }
        total_concepts += concepts;
        total_questions += questions;
        free(unit_label);
    }
    if (error == LBDB_OK && !lbdb_json_end_array(imported)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize import output");
    }
    if (error == LBDB_OK) {
        char details[160] = {0};
        const int length = snprintf(
            details, sizeof(details), "{\"files\":%zu,\"concepts\":%lld,\"questions\":%lld}",
            files.count, (long long)total_concepts, (long long)total_questions);
        if (length <= 0 || (size_t)length >= sizeof(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format bank import audit");
        } else {
            error = lbdb_commit_write(app, database, "bank.import", "bank", 0, details);
        }
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.import");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "imported"));
        LBDB_JSON(app, lbdb_json_raw(app->output, lbdb_json_data(imported)));
        LBDB_JSON(app, lbdb_json_key(app->output, "concepts"));
        LBDB_JSON(app, lbdb_json_int(app->output, total_concepts));
        LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
        LBDB_JSON(app, lbdb_json_int(app->output, total_questions));
        error = lbdb_output_end(app);
    }
    lbdb_json_writer_destroy(imported);
    lbdb_database_close(database);
    bank_files_destroy(&files);
    lbdb_string_vector_destroy(&paths);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError write_sources_json(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                    const char *table, const char *owner_column, int64_t owner_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    char *sql = lbdb_string_format(
        "SELECT l.position,s.section_key,l.source_pages,l.source_line_start,"
        "l.source_line_end,l.role,l.justification FROM %s l "
        "JOIN source_sections s ON s.id=l.section_id WHERE l.%s=?1 ORDER BY l.position",
        table, owner_column);
    if (sql == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source export query");
    }
    LbdbError error = lbdb_statement_prepare(database, sql, &statement);
    free(sql);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, owner_id);
    }
    if (error == LBDB_OK && !lbdb_json_begin_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build source export");
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "position") ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
            !lbdb_json_key(writer, "section_key") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
            !lbdb_json_key(writer, "source_pages") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
            !lbdb_json_key(writer, "source_line_start") ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 3)) ||
            !lbdb_json_key(writer, "source_line_end") ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 4)) ||
            !lbdb_json_key(writer, "role") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 5)) ||
            !lbdb_json_key(writer, "justification") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 6)) ||
            !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build source export");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot export source links");
    }
    if (error == LBDB_OK && !lbdb_json_end_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize source export");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError write_question_tags_json(LbdbApp *app, LbdbDatabase *database,
                                          LbdbJsonWriter *writer, int64_t question_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT t.kind,t.name,t.description FROM tags t JOIN question_tags qt ON qt.tag_id=t.id "
        "WHERE qt.question_id=?1 ORDER BY t.kind COLLATE NOCASE,t.name COLLATE NOCASE",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK && !lbdb_json_begin_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag export");
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "kind") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 0)) ||
            !lbdb_json_key(writer, "name") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
            !lbdb_json_key(writer, "description") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
            !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag export");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot export question tags");
    }
    if (error == LBDB_OK && !lbdb_json_end_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize tag export");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError write_concept_tag_union(LbdbApp *app, LbdbDatabase *database,
                                         LbdbJsonWriter *writer, int64_t concept_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error =
        lbdb_statement_prepare(database,
                               "SELECT DISTINCT t.kind,t.name,t.description FROM tags t "
                               "JOIN question_tags qt ON qt.tag_id=t.id "
                               "JOIN question_bank q ON q.id=qt.question_id WHERE q.concept_id=?1 "
                               "ORDER BY t.kind COLLATE NOCASE,t.name COLLATE NOCASE",
                               &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, concept_id);
    }
    if (error == LBDB_OK && !lbdb_json_begin_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept tag export");
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "kind") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 0)) ||
            !lbdb_json_key(writer, "name") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
            !lbdb_json_key(writer, "description") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
            !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept tag export");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot export concept tags");
    }
    if (error == LBDB_OK && !lbdb_json_end_array(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize concept tag export");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError write_export_question(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                       LbdbStatement *question) {
    const int64_t question_id = lbdb_statement_column_int64(question, 0);
    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "question_type") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 1)) ||
        !lbdb_json_key(writer, "response_format") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 2)) ||
        !lbdb_json_key(writer, "prompt") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 3)) ||
        !lbdb_json_key(writer, "options")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question export");
    }
    if (lbdb_statement_column_is_null(question, 4)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question export");
        }
    } else if (!lbdb_json_raw(writer, lbdb_statement_column_text(question, 4))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question export");
    }
    if (!lbdb_json_key(writer, "expected_answer") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 5)) ||
        !lbdb_json_key(writer, "grading_criteria") ||
        !lbdb_json_raw(writer, lbdb_statement_column_text(question, 6)) ||
        !lbdb_json_key(writer, "answer_justification") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 7)) ||
        !lbdb_json_key(writer, "source_section") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 8)) ||
        !lbdb_json_key(writer, "source_pages") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(question, 9)) ||
        !lbdb_json_key(writer, "source_line_start") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(question, 10)) ||
        !lbdb_json_key(writer, "source_line_end") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(question, 11)) ||
        !lbdb_json_key(writer, "sources")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question export");
    }
    LbdbError error =
        write_sources_json(app, database, writer, "question_sources", "question_id", question_id);
    if (error == LBDB_OK && !lbdb_json_key(writer, "tags")) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question export");
    }
    if (error == LBDB_OK) {
        error = write_question_tags_json(app, database, writer, question_id);
    }
    if (error == LBDB_OK &&
        (!lbdb_json_key(writer, "active") ||
         !lbdb_json_bool(writer, lbdb_statement_column_int64(question, 12) != 0) ||
         !lbdb_json_key(writer, "revision") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(question, 13)) ||
         !lbdb_json_end_object(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize question export");
    }
    return error;
}

static LbdbError build_bank_export(LbdbApp *app, LbdbDatabase *database, int64_t unit_id,
                                   LbdbJsonWriter *writer, int64_t *concept_count,
                                   int64_t *question_count) {
    LbdbStatement *unit = NULL;
    LbdbStatement *concepts = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database, "SELECT corpus_slug,unit_key FROM source_units WHERE id=?1", &unit);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(unit, 1, unit_id);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, unit, &has_row, "Cannot load export unit");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown source unit id: %lld",
                              (long long)unit_id);
    }
    if (error == LBDB_OK &&
        (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "format_version") ||
         !lbdb_json_int(writer, 1) || !lbdb_json_key(writer, "book_slug") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(unit, 0)) ||
         !lbdb_json_key(writer, "unit_key") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(unit, 1)) ||
         !lbdb_json_key(writer, "concepts") || !lbdb_json_begin_array(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to initialize bank export");
    }
    lbdb_statement_destroy(unit);
    unit = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,name,description,importance,theory_required,application_required,"
            "code_required,semantics_required,source_pages,source_line_start,source_line_end,"
            "justification FROM concepts WHERE unit_id=?1 ORDER BY position",
            &concepts);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(concepts, 1, unit_id);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(concepts, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const int64_t concept_id = lbdb_statement_column_int64(concepts, 0);
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "name") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(concepts, 1)) ||
            !lbdb_json_key(writer, "description") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(concepts, 2)) ||
            !lbdb_json_key(writer, "importance") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(concepts, 3)) ||
            !lbdb_json_key(writer, "coverage") || !lbdb_json_begin_array(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept export");
            break;
        }
        static const char *const coverage_names[] = {"theory", "application", "code", "semantics"};
        for (int column = 4; error == LBDB_OK && column <= 7; ++column) {
            if (lbdb_statement_column_int64(concepts, column) != 0 &&
                !lbdb_json_string(writer, coverage_names[column - 4])) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept coverage");
            }
        }
        if (error == LBDB_OK &&
            (!lbdb_json_end_array(writer) || !lbdb_json_key(writer, "source_pages") ||
             !lbdb_json_string(writer, lbdb_statement_column_text(concepts, 8)) ||
             !lbdb_json_key(writer, "source_line_start") ||
             !lbdb_json_int(writer, lbdb_statement_column_int64(concepts, 9)) ||
             !lbdb_json_key(writer, "source_line_end") ||
             !lbdb_json_int(writer, lbdb_statement_column_int64(concepts, 10)) ||
             !lbdb_json_key(writer, "justification") ||
             !lbdb_json_string(writer, lbdb_statement_column_text(concepts, 11)) ||
             !lbdb_json_key(writer, "sources"))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept export");
        }
        if (error == LBDB_OK) {
            error = write_sources_json(app, database, writer, "concept_sources", "concept_id",
                                       concept_id);
        }
        if (error == LBDB_OK && !lbdb_json_key(writer, "tags")) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build concept export");
        }
        if (error == LBDB_OK) {
            error = write_concept_tag_union(app, database, writer, concept_id);
        }
        if (error == LBDB_OK &&
            (!lbdb_json_key(writer, "questions") || !lbdb_json_begin_array(writer))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question list");
        }
        LbdbStatement *questions = NULL;
        bool question_row = false;
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database,
                "SELECT id,question_type,response_format,prompt,options_json,expected_answer,"
                "grading_criteria_json,answer_justification,source_section,source_pages,"
                "source_line_start,source_line_end,active,revision FROM question_bank "
                "WHERE concept_id=?1 ORDER BY position",
                &questions);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(questions, 1, concept_id);
        }
        while (error == LBDB_OK) {
            error = lbdb_statement_step(questions, &question_row);
            if (error != LBDB_OK || !question_row) {
                break;
            }
            error = write_export_question(app, database, writer, questions);
            if (error == LBDB_OK) {
                *question_count += 1;
            }
        }
        if (error == LBDB_ERROR_SQLITE) {
            error = lbdb_app_database_error(app, database, error, "Cannot export questions");
        }
        lbdb_statement_destroy(questions);
        if (error == LBDB_OK && (!lbdb_json_end_array(writer) || !lbdb_json_end_object(writer))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize concept export");
        }
        if (error == LBDB_OK) {
            *concept_count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot export concepts");
    }
    if (error == LBDB_OK && (!lbdb_json_end_array(writer) || !lbdb_json_end_object(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize bank export");
    }
    lbdb_statement_destroy(concepts);
    return error;
}

LbdbError lbdb_command_bank_export(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *unit_reference = NULL;
    const char *output_value = NULL;
    char *output = NULL;
    LbdbDatabase *database = NULL;
    LbdbJsonWriter *writer = NULL;
    int64_t unit_id = 0;
    int64_t concepts = 0;
    int64_t questions = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--unit", true, &unit_reference);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--output", true, &output_value);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_path(app, output_value, false, false, &output);
    }
    if (error == LBDB_OK && lbdb_path_exists(output)) {
        error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Export destination exists: %s", output);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_unit_id(app, database, unit_reference, &unit_id);
    }
    writer = lbdb_json_writer_create(true);
    if (error == LBDB_OK && writer == NULL) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate export writer");
    }
    if (error == LBDB_OK) {
        error = build_bank_export(app, database, unit_id, writer, &concepts, &questions);
    }
    if (error == LBDB_OK) {
        error = lbdb_write_json_file_exclusive(app, output, writer);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.export");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "unit_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, unit_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "output"));
        LBDB_JSON(app, lbdb_json_string(app->output, output));
        LBDB_JSON(app, lbdb_json_key(app->output, "concepts"));
        LBDB_JSON(app, lbdb_json_int(app->output, concepts));
        LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
        LBDB_JSON(app, lbdb_json_int(app->output, questions));
        error = lbdb_output_end(app);
    }
    lbdb_json_writer_destroy(writer);
    lbdb_database_close(database);
    free(output);
    lbdb_args_destroy(&args);
    return error;
}

static char *safe_export_filename(const char *slug, const char *key) {
    char *combined = lbdb_string_format("%s--%s.json", slug, key);
    if (combined == NULL) {
        return NULL;
    }
    for (char *cursor = combined; *cursor != '\0'; ++cursor) {
        const unsigned char character = (unsigned char)*cursor;
        if (!isalnum(character) && character != '-' && character != '_' && character != '.') {
            *cursor = '-';
        }
    }
    return combined;
}

LbdbError lbdb_command_bank_export_all(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *directory_value = NULL;
    char *directory = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *units = NULL;
    LbdbStringVector paths = {0};
    int64_t export_count = 0;
    bool has_row = false;
    bool transaction_started = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--output-dir", true, &directory_value);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_path(app, directory_value, false, false, &directory);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_database_exec_static(database, "BEGIN");
        if (error == LBDB_OK) {
            transaction_started = true;
        } else {
            (void)lbdb_app_database_error(app, database, error,
                                          "Cannot start bank export snapshot");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,corpus_slug,unit_key FROM source_units su WHERE EXISTS("
            "SELECT 1 FROM concepts c WHERE c.unit_id=su.id) ORDER BY corpus_slug,position",
            &units);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(units, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        char *filename = safe_export_filename(lbdb_statement_column_text(units, 1),
                                              lbdb_statement_column_text(units, 2));
        char *path = filename != NULL ? lbdb_string_format("%s/%s", directory, filename) : NULL;
        free(filename);
        if (path == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate export path");
            break;
        }
        if (lbdb_path_exists(path)) {
            error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Export destination exists: %s", path);
            free(path);
            break;
        }
        error = lbdb_string_vector_push(&paths, path);
        free(path);
    }
    if (error == LBDB_ERROR_SQLITE) {
        (void)lbdb_app_database_error(app, database, error, "Cannot enumerate bank exports");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_reset(units);
    }
    size_t path_index = 0U;
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.export-all");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "output_dir"));
        LBDB_JSON(app, lbdb_json_string(app->output, directory));
        LBDB_JSON(app, lbdb_json_key(app->output, "exports"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(units, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        LbdbJsonWriter *writer = lbdb_json_writer_create(true);
        int64_t concepts = 0;
        int64_t questions = 0;
        const int64_t unit_id = lbdb_statement_column_int64(units, 0);
        if (writer == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate export writer");
        }
        if (error == LBDB_OK) {
            error = build_bank_export(app, database, unit_id, writer, &concepts, &questions);
        }
        if (error == LBDB_OK && path_index >= paths.count) {
            (void)lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Bank export plan changed unexpectedly");
            error = LBDB_ERROR_INTERNAL;
        }
        if (error == LBDB_OK) {
            error = lbdb_write_json_file_exclusive(app, paths.items[path_index], writer);
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_begin_object(app->output));
            LBDB_JSON(app, lbdb_json_key(app->output, "unit_id"));
            LBDB_JSON(app, lbdb_json_int(app->output, unit_id));
            LBDB_JSON(app, lbdb_json_key(app->output, "output"));
            LBDB_JSON(app, lbdb_json_string(app->output, paths.items[path_index]));
            LBDB_JSON(app, lbdb_json_key(app->output, "concepts"));
            LBDB_JSON(app, lbdb_json_int(app->output, concepts));
            LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
            LBDB_JSON(app, lbdb_json_int(app->output, questions));
            LBDB_JSON(app, lbdb_json_end_object(app->output));
            path_index += 1U;
            export_count += 1;
        }
        lbdb_json_writer_destroy(writer);
    }
    if (error == LBDB_ERROR_SQLITE) {
        (void)lbdb_app_database_error(app, database, error, "Cannot enumerate bank exports");
    }
    if (transaction_started) {
        if (error == LBDB_OK) {
            error = lbdb_database_commit(database);
            if (error != LBDB_OK) {
                (void)lbdb_app_database_error(app, database, error,
                                              "Cannot finish bank export snapshot");
            }
        } else {
            lbdb_database_rollback(database);
        }
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, export_count));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(units);
    lbdb_database_close(database);
    lbdb_string_vector_destroy(&paths);
    free(directory);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError scalar_count(LbdbApp *app, LbdbDatabase *database, const char *sql,
                              int64_t *count) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(database, sql, &statement);
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot run validation query");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Validation query returned no row");
    }
    if (error == LBDB_OK) {
        *count = lbdb_statement_column_int64(statement, 0);
    }
    lbdb_statement_destroy(statement);
    return error;
}

LbdbError lbdb_command_bank_validate(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    bool allow_incomplete = false;
    LbdbDatabase *database = NULL;
    LbdbStatement *units = NULL;
    bool has_row = false;
    int64_t source_errors = 0;
    int64_t missing_concepts = 0;
    int64_t invalid_concept_sources = 0;
    int64_t invalid_question_sources = 0;
    int64_t missing_tag_kinds = 0;
    int64_t concepts_without_questions = 0;
    int64_t uncovered_sections = 0;
    int64_t coverage_errors = 0;
    int64_t template_errors = 0;
    int64_t counts[4] = {0};
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_flag(&args, "--allow-incomplete", &allow_incomplete);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT source_path,content_sha256 FROM source_units "
                                       "ORDER BY corpus_slug,position",
                                       &units);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(units, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const char *stored_path = lbdb_statement_column_text(units, 0);
        char *path = NULL;
        char digest[65] = {0};
        error = lbdb_resolve_path(app, stored_path, true, true, &path);
        if (error != LBDB_OK) {
            clearerr(stderr);
            source_errors += 1;
            free(app->error_message);
            free(app->error_details);
            app->error_message = NULL;
            app->error_details = NULL;
            app->error = LBDB_OK;
            error = LBDB_OK;
            continue;
        }
        error = lbdb_file_sha256(app, path, digest);
        if (error == LBDB_OK && strcmp(digest, lbdb_statement_column_text(units, 1)) != 0) {
            source_errors += 1;
        }
        free(path);
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot validate source units");
    }
    lbdb_statement_destroy(units);
    units = NULL;
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM source_units su WHERE su.include_in_quizzes=1 AND NOT EXISTS("
            "SELECT 1 FROM concepts c WHERE c.unit_id=su.id)",
            &missing_concepts);
    }
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM concepts c WHERE NOT EXISTS(SELECT 1 FROM concept_sources cs "
            "WHERE cs.concept_id=c.id) OR EXISTS(SELECT 1 FROM concept_sources cs "
            "JOIN source_sections s ON s.id=cs.section_id WHERE cs.concept_id=c.id AND "
            "(s.is_summary=1 OR s.unit_id<>c.unit_id OR cs.source_line_start<s.start_line OR "
            "cs.source_line_end>s.end_line OR (instr(c.source_pages,cs.source_pages)=0 AND "
            "instr(cs.source_pages,c.source_pages)=0))) OR c.source_line_start<>(SELECT "
            "min(cs.source_line_start) FROM concept_sources cs WHERE cs.concept_id=c.id) OR "
            "c.source_line_end<>(SELECT max(cs.source_line_end) FROM concept_sources cs WHERE "
            "cs.concept_id=c.id) OR c.primary_section_id<>(SELECT min(cs.section_id) FROM "
            "concept_sources cs WHERE cs.concept_id=c.id AND cs.role='primary') OR "
            "(SELECT count(*) FROM concept_sources cs WHERE cs.concept_id=c.id AND "
            "cs.role='primary')<>1",
            &invalid_concept_sources);
    }
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM question_bank q WHERE NOT EXISTS(SELECT 1 FROM question_sources "
            "qs WHERE qs.question_id=q.id) OR EXISTS(SELECT 1 FROM question_sources qs "
            "JOIN source_sections s ON s.id=qs.section_id WHERE qs.question_id=q.id AND "
            "(s.is_summary=1 OR s.unit_id<>q.unit_id OR qs.source_line_start<s.start_line OR "
            "qs.source_line_end>s.end_line OR (instr(q.source_pages,qs.source_pages)=0 AND "
            "instr(qs.source_pages,q.source_pages)=0))) OR q.source_line_start<>(SELECT "
            "min(qs.source_line_start) FROM question_sources qs WHERE qs.question_id=q.id) OR "
            "q.source_line_end<>(SELECT max(qs.source_line_end) FROM question_sources qs WHERE "
            "qs.question_id=q.id) OR q.source_section<>(SELECT min(s.title) FROM question_sources "
            "qs JOIN source_sections s ON s.id=qs.section_id WHERE qs.question_id=q.id AND "
            "qs.role='primary') OR (SELECT count(*) FROM question_sources qs WHERE "
            "qs.question_id=q.id AND qs.role='primary')<>1",
            &invalid_question_sources);
    }
    if (error == LBDB_OK) {
        error =
            scalar_count(app, database,
                         "SELECT count(*) FROM question_bank q WHERE q.active=1 AND ("
                         "NOT EXISTS(SELECT 1 FROM question_tags qt JOIN tags t ON t.id=qt.tag_id "
                         "WHERE qt.question_id=q.id AND lower(t.kind)='topic') OR "
                         "NOT EXISTS(SELECT 1 FROM question_tags qt JOIN tags t ON t.id=qt.tag_id "
                         "WHERE qt.question_id=q.id AND lower(t.kind)='theme') OR "
                         "NOT EXISTS(SELECT 1 FROM question_tags qt JOIN tags t ON t.id=qt.tag_id "
                         "WHERE qt.question_id=q.id AND lower(t.kind)='mode'))",
                         &missing_tag_kinds);
    }
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM concepts c WHERE NOT EXISTS(SELECT 1 FROM question_bank q "
            "WHERE q.concept_id=c.id AND q.active=1)",
            &concepts_without_questions);
    }
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM source_sections s JOIN source_units u ON u.id=s.unit_id "
            "WHERE u.include_in_quizzes=1 AND s.is_summary=0 AND "
            "coalesce(json_extract(s.metadata_json,'$.coverage_exempt'),0)<>1 AND "
            "EXISTS(SELECT 1 FROM concepts c WHERE c.unit_id=u.id) AND NOT EXISTS("
            "SELECT 1 FROM concept_sources cs WHERE cs.section_id=s.id)",
            &uncovered_sections);
    }
    if (error == LBDB_OK) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM concepts c WHERE "
            "(c.theory_required=1 AND NOT EXISTS(SELECT 1 FROM question_bank q WHERE "
            "q.concept_id=c.id AND q.active=1 AND q.question_type IN('recall','relationship',"
            "'misconception','code_reading'))) OR "
            "(c.application_required=1 AND NOT EXISTS(SELECT 1 FROM question_bank q WHERE "
            "q.concept_id=c.id AND q.active=1 AND q.question_type IN('application','example',"
            "'misconception','code_reading','code_writing','debugging'))) OR "
            "(c.code_required=1 AND NOT EXISTS(SELECT 1 FROM question_bank q WHERE "
            "q.concept_id=c.id AND q.active=1 AND q.question_type IN('code_reading','code_writing',"
            "'debugging'))) OR "
            "(c.semantics_required=1 AND NOT EXISTS(SELECT 1 FROM question_bank q WHERE "
            "q.concept_id=c.id AND q.active=1 AND q.question_type IN('recall','relationship',"
            "'misconception','code_reading','debugging')))",
            &coverage_errors);
    }
    if (error == LBDB_OK && !allow_incomplete) {
        error = scalar_count(
            app, database,
            "SELECT count(*) FROM source_units u WHERE u.include_in_quizzes=1 AND EXISTS("
            "SELECT 1 FROM concepts c WHERE c.unit_id=u.id) AND ("
            "(SELECT count(*) FROM quiz_templates t WHERE t.unit_id=u.id AND t.active=1 "
            "AND t.scope_type IN('checkpoint','final'))<>4 OR "
            "(SELECT count(*) FROM quiz_template_questions tq JOIN quiz_templates t ON "
            "t.id=tq.template_id WHERE t.unit_id=u.id AND t.scope_type='final' AND t.active=1)<>"
            "(SELECT count(*) FROM question_bank q WHERE q.unit_id=u.id AND q.active=1))",
            &template_errors);
        if (error == LBDB_OK) {
            int64_t missing_tag_templates = 0;
            error = scalar_count(
                app, database,
                "SELECT count(*) FROM tags t WHERE lower(t.kind) IN('topic','theme') AND EXISTS("
                "SELECT 1 FROM question_tags qt JOIN question_bank q ON q.id=qt.question_id "
                "WHERE qt.tag_id=t.id AND q.active=1) AND NOT EXISTS(SELECT 1 FROM quiz_templates "
                "x WHERE x.tag_id=t.id AND x.active=1)",
                &missing_tag_templates);
            template_errors += missing_tag_templates;
        }
    }
    if (error == LBDB_OK) {
        error = scalar_count(app, database, "SELECT count(*) FROM source_units", &counts[0]);
    }
    if (error == LBDB_OK) {
        error = scalar_count(app, database, "SELECT count(*) FROM concepts", &counts[1]);
    }
    if (error == LBDB_OK) {
        error = scalar_count(app, database, "SELECT count(*) FROM question_bank WHERE active=1",
                             &counts[2]);
    }
    if (error == LBDB_OK) {
        error = scalar_count(app, database, "SELECT count(*) FROM quiz_templates WHERE active=1",
                             &counts[3]);
    }
    const int64_t structural_errors =
        source_errors + invalid_concept_sources + invalid_question_sources + missing_tag_kinds;
    const int64_t completeness_errors = missing_concepts + concepts_without_questions +
                                        uncovered_sections + coverage_errors + template_errors;
    const int64_t total_errors = structural_errors + (allow_incomplete ? 0 : completeness_errors);
    if (error == LBDB_OK && total_errors > 0) {
        char details[512] = {0};
        const int length = snprintf(
            details, sizeof(details),
            "{\"source_errors\":%lld,\"missing_concepts\":%lld,"
            "\"invalid_concept_sources\":%lld,\"invalid_question_sources\":%lld,"
            "\"missing_tag_kinds\":%lld,\"concepts_without_questions\":%lld,"
            "\"uncovered_sections\":%lld,\"coverage_errors\":%lld,"
            "\"template_errors\":%lld}",
            (long long)source_errors, (long long)missing_concepts,
            (long long)invalid_concept_sources, (long long)invalid_question_sources,
            (long long)missing_tag_kinds, (long long)concepts_without_questions,
            (long long)uncovered_sections, (long long)coverage_errors, (long long)template_errors);
        error = length > 0 && (size_t)length < sizeof(details)
                    ? lbdb_app_fail_details(app, LBDB_ERROR_VALIDATION, details,
                                            "Bank validation failed with %lld error(s)",
                                            (long long)total_errors)
                    : lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Bank validation failed");
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.validate");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "valid"));
        LBDB_JSON(app, lbdb_json_bool(app->output, true));
        LBDB_JSON(app, lbdb_json_key(app->output, "allow_incomplete"));
        LBDB_JSON(app, lbdb_json_bool(app->output, allow_incomplete));
        LBDB_JSON(app, lbdb_json_key(app->output, "counts"));
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        static const char *const count_names[] = {"units", "concepts", "active_questions",
                                                  "active_templates"};
        for (size_t index = 0; index < 4U; ++index) {
            LBDB_JSON(app, lbdb_json_key(app->output, count_names[index]));
            LBDB_JSON(app, lbdb_json_int(app->output, counts[index]));
        }
        LBDB_JSON(app, lbdb_json_end_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "warnings"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(units);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError write_bank_question(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                     int64_t question_id, bool admin) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id,unit_id,concept_id,position,question_type,response_format,prompt,options_json,"
        "expected_answer,grading_criteria_json,answer_justification,source_section,source_pages,"
        "source_line_start,source_line_end,active,revision,earliest_checkpoint,body_verified "
        "FROM question_bank WHERE id=?1",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot load bank question");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown bank question: %lld",
                              (long long)question_id);
    }
    if (error == LBDB_OK &&
        (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
         !lbdb_json_key(writer, "unit_id") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 1)) ||
         !lbdb_json_key(writer, "concept_id") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 2)) ||
         !lbdb_json_key(writer, "position") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 3)) ||
         !lbdb_json_key(writer, "question_type") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 4)) ||
         !lbdb_json_key(writer, "response_format") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 5)) ||
         !lbdb_json_key(writer, "prompt") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 6)) ||
         !lbdb_json_key(writer, "source_section") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 11)) ||
         !lbdb_json_key(writer, "source_pages") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 12)) ||
         !lbdb_json_key(writer, "source_line_start") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 13)) ||
         !lbdb_json_key(writer, "source_line_end") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 14)) ||
         !lbdb_json_key(writer, "active") ||
         !lbdb_json_bool(writer, lbdb_statement_column_int64(statement, 15) != 0) ||
         !lbdb_json_key(writer, "revision") ||
         !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 16)) ||
         !lbdb_json_key(writer, "tags"))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
    }
    if (error == LBDB_OK) {
        error = write_question_tags_json(app, database, writer, question_id);
    }
    if (error == LBDB_OK && admin) {
        if (!lbdb_json_key(writer, "options")) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
        } else if (lbdb_statement_column_is_null(statement, 7)) {
            if (!lbdb_json_null(writer)) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
            }
        } else if (!lbdb_json_raw(writer, lbdb_statement_column_text(statement, 7))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
        }
        if (error == LBDB_OK &&
            (!lbdb_json_key(writer, "expected_answer") ||
             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 8)) ||
             !lbdb_json_key(writer, "grading_criteria") ||
             !lbdb_json_raw(writer, lbdb_statement_column_text(statement, 9)) ||
             !lbdb_json_key(writer, "answer_justification") ||
             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 10)) ||
             !lbdb_json_key(writer, "sources"))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
        }
        if (error == LBDB_OK) {
            error = write_sources_json(app, database, writer, "question_sources", "question_id",
                                       question_id);
        }
        if (error == LBDB_OK &&
            (!lbdb_json_key(writer, "earliest_checkpoint") ||
             !lbdb_json_double(writer, lbdb_statement_column_double(statement, 17)) ||
             !lbdb_json_key(writer, "body_verified") ||
             !lbdb_json_bool(writer, lbdb_statement_column_int64(statement, 18) != 0))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank question output");
        }
    }
    if (error == LBDB_OK && !lbdb_json_end_object(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize bank question output");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError repeated_option(LbdbArgs *args, const char *name, LbdbStringVector *values) {
    for (int index = 0; index < args->argc; ++index) {
        if (args->used[index] || strcmp(args->argv[index], name) != 0) {
            continue;
        }
        if (index + 1 >= args->argc || args->used[index + 1]) {
            return lbdb_app_fail(args->app, LBDB_ERROR_USAGE, "Missing value for %s", name);
        }
        LbdbError error = lbdb_string_vector_push(values, args->argv[index + 1]);
        if (error != LBDB_OK) {
            return lbdb_app_fail(args->app, error, "Unable to store %s value", name);
        }
        args->used[index] = true;
        args->used[index + 1] = true;
        index += 1;
    }
    return LBDB_OK;
}

static LbdbError append_sql(LbdbApp *app, char **sql, const char *fragment) {
    char *combined = lbdb_string_format("%s%s", *sql, fragment);
    if (combined == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate search query");
    }
    free(*sql);
    *sql = combined;
    return LBDB_OK;
}

LbdbError lbdb_command_bank_search(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbStringVector tags = {0};
    const char *unit_ref = NULL;
    const char *tag_kind = NULL;
    const char *question_type = NULL;
    const char *response_format = NULL;
    const char *active = NULL;
    const char *text = NULL;
    const char *limit_text = NULL;
    int64_t limit = 100;
    int64_t unit_id = 0;
    int64_t *tag_ids = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    char *sql = NULL;
    bool has_row = false;
    int64_t count = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = repeated_option(&args, "--tag", &tags);
    }
#define SEARCH_OPTION(name, target)                                                                \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_args_option(&args, (name), false, &(target));                             \
        }                                                                                          \
    } while (false)
    SEARCH_OPTION("--unit", unit_ref);
    SEARCH_OPTION("--tag-kind", tag_kind);
    SEARCH_OPTION("--question-type", question_type);
    SEARCH_OPTION("--response-format", response_format);
    SEARCH_OPTION("--active", active);
    SEARCH_OPTION("--text", text);
    SEARCH_OPTION("--limit", limit_text);
#undef SEARCH_OPTION
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && limit_text != NULL) {
        error = lbdb_parse_positive_int(app, "--limit", limit_text, &limit);
    }
    if (active == NULL) {
        active = "active";
    }
    static const char *const active_values[] = {"active", "all", "retired"};
    if (error == LBDB_OK && !lbdb_string_in_set(active, active_values,
                                                sizeof(active_values) / sizeof(active_values[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "--active must be active, retired, or all");
    }
    if (error == LBDB_OK && question_type != NULL &&
        !lbdb_string_in_set(question_type, question_types,
                            sizeof(question_types) / sizeof(question_types[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown question type: %s", question_type);
    }
    if (error == LBDB_OK && response_format != NULL &&
        !lbdb_string_in_set(response_format, response_formats,
                            sizeof(response_formats) / sizeof(response_formats[0]))) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown response format: %s", response_format);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK && unit_ref != NULL) {
        error = lbdb_resolve_unit_id(app, database, unit_ref, &unit_id);
    }
    if (error == LBDB_OK && tags.count > 0U) {
        tag_ids = calloc(tags.count, sizeof(*tag_ids));
        if (tag_ids == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate tag filters");
        }
    }
    for (size_t index = 0; error == LBDB_OK && index < tags.count; ++index) {
        error = lbdb_resolve_tag_id(app, database, tags.items[index], &tag_ids[index]);
    }
    if (error == LBDB_OK) {
        sql = lbdb_string_duplicate("SELECT q.id FROM question_bank q WHERE 1=1");
        if (sql == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate search query");
        }
    }
    if (error == LBDB_OK && unit_ref != NULL) {
        error = append_sql(app, &sql, " AND q.unit_id=?");
    }
    if (error == LBDB_OK && question_type != NULL) {
        error = append_sql(app, &sql, " AND q.question_type=?");
    }
    if (error == LBDB_OK && response_format != NULL) {
        error = append_sql(app, &sql, " AND q.response_format=?");
    }
    if (error == LBDB_OK && strcmp(active, "active") == 0) {
        error = append_sql(app, &sql, " AND q.active=1");
    } else if (error == LBDB_OK && strcmp(active, "retired") == 0) {
        error = append_sql(app, &sql, " AND q.active=0");
    }
    if (error == LBDB_OK && text != NULL) {
        error = append_sql(app, &sql, " AND (q.prompt LIKE ? OR q.expected_answer LIKE ?)");
    }
    if (error == LBDB_OK && tag_kind != NULL) {
        error = append_sql(app, &sql,
                           " AND EXISTS(SELECT 1 FROM question_tags x JOIN tags t ON t.id=x.tag_id "
                           "WHERE x.question_id=q.id AND t.kind=?)");
    }
    for (size_t index = 0; error == LBDB_OK && index < tags.count; ++index) {
        error = append_sql(app, &sql,
                           " AND EXISTS(SELECT 1 FROM question_tags x WHERE x.question_id=q.id "
                           "AND x.tag_id=?)");
    }
    if (error == LBDB_OK) {
        error = append_sql(app, &sql, " ORDER BY q.unit_id,q.position LIMIT ?");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database, sql, &statement);
    }
    int bind = 1;
    if (error == LBDB_OK && unit_ref != NULL) {
        error = lbdb_statement_bind_int64(statement, bind++, unit_id);
    }
    if (error == LBDB_OK && question_type != NULL) {
        error = lbdb_statement_bind_text(statement, bind++, question_type);
    }
    if (error == LBDB_OK && response_format != NULL) {
        error = lbdb_statement_bind_text(statement, bind++, response_format);
    }
    char *pattern = NULL;
    if (error == LBDB_OK && text != NULL) {
        pattern = lbdb_string_format("%%%s%%", text);
        if (pattern == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate search pattern");
        } else {
            error = lbdb_statement_bind_text(statement, bind++, pattern);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, bind++, pattern);
        }
    }
    if (error == LBDB_OK && tag_kind != NULL) {
        error = lbdb_statement_bind_text(statement, bind++, tag_kind);
    }
    for (size_t index = 0; error == LBDB_OK && index < tags.count; ++index) {
        error = lbdb_statement_bind_int64(statement, bind++, tag_ids[index]);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, bind, limit);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.search");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = write_bank_question(app, database, app->output,
                                    lbdb_statement_column_int64(statement, 0), false);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot search bank questions");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        error = lbdb_output_end(app);
    }
    free(pattern);
    free(sql);
    free(tag_ids);
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_string_vector_destroy(&tags);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError one_positive_positional(LbdbCommand *command, LbdbArgs *args, int64_t *id) {
    LbdbStringVector values = {0};
    LbdbError error = lbdb_args_positionals(args, &values);
    if (error == LBDB_OK && values.count != 1U) {
        error = lbdb_app_fail(command->app, LBDB_ERROR_USAGE, "%s requires exactly one positive ID",
                              command->key);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(command->app, "ID", values.items[0], id);
    }
    lbdb_string_vector_destroy(&values);
    return error;
}

LbdbError lbdb_command_bank_show(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbDatabase *database = NULL;
    int64_t question_id = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = one_positive_positional(command, &args, &question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.show");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "question"));
        error = write_bank_question(app, database, app->output, question_id, true);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError json_argument(LbdbApp *app, const char *argument, char **json) {
    if (argument[0] == '@') {
        char *path = NULL;
        size_t size = 0U;
        LbdbError error = lbdb_resolve_path(app, argument + 1, true, false, &path);
        if (error == LBDB_OK) {
            error = lbdb_read_file(app, path, json, &size);
        }
        (void)size;
        free(path);
        return error;
    }
    *json = lbdb_string_duplicate(argument);
    return *json != NULL ? LBDB_OK
                         : lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate JSON input");
}

static LbdbError validate_revision_keys(LbdbApp *app, LbdbDatabase *database, const char *json) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT group_concat(key,',') FROM json_each(?1) WHERE key NOT IN("
        "'question_type','response_format','prompt','options','expected_answer',"
        "'grading_criteria','answer_justification','source_section','source_pages',"
        "'source_line_start','source_line_end','sources','tags','reason')",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, json);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot validate revision keys");
    }
    if (error == LBDB_OK && has_row && !lbdb_statement_column_is_null(statement, 0)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unknown revision fields: %s",
                              lbdb_statement_column_text(statement, 0));
    }
    lbdb_statement_destroy(statement);
    return error;
}

LbdbError lbdb_command_bank_revise(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *input = NULL;
    int64_t question_id = 0;
    char *input_json = NULL;
    char *reason = NULL;
    char *merged_json = NULL;
    char *tags_json = NULL;
    LbdbQuestionData question = {0};
    LbdbSectionRanges ranges = {0};
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t unit_id = 0;
    int64_t unit_start = 0;
    int64_t unit_end = 0;
    int64_t revision = 0;
    bool source_changed = false;
    bool tags_changed = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--input", true, &input);
    }
    if (error == LBDB_OK) {
        error = one_positive_positional(command, &args, &question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = json_argument(app, input, &input_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_document_type(app, database, input_json, "object");
    }
    if (error == LBDB_OK) {
        error = validate_revision_keys(app, database, input_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_text(app, database, input_json, "$.reason", true, &reason);
        if (error == LBDB_OK && !nonempty_text(reason)) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Revision reason is required");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT q.unit_id,u.start_line,u.end_line,q.revision,"
            "json_patch(json_object('question_type',q.question_type,'response_format',"
            "q.response_format,'prompt',q.prompt,'options',CASE WHEN q.options_json IS NULL "
            "THEN NULL ELSE json(q.options_json) END,'expected_answer',q.expected_answer,"
            "'grading_criteria',json(q.grading_criteria_json),'answer_justification',"
            "q.answer_justification,'source_section',q.source_section,'source_pages',"
            "q.source_pages,'source_line_start',q.source_line_start,'source_line_end',"
            "q.source_line_end),?2),"
            "EXISTS(SELECT 1 FROM json_each(?2) WHERE key IN('source_section','source_pages',"
            "'source_line_start','source_line_end','answer_justification','sources')),"
            "EXISTS(SELECT 1 FROM json_each(?2) WHERE key='tags') "
            "FROM question_bank q JOIN source_units u ON u.id=q.unit_id WHERE q.id=?1",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, input_json);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot load bank question");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown bank question: %lld",
                              (long long)question_id);
    }
    if (error == LBDB_OK) {
        unit_id = lbdb_statement_column_int64(statement, 0);
        unit_start = lbdb_statement_column_int64(statement, 1);
        unit_end = lbdb_statement_column_int64(statement, 2);
        revision = lbdb_statement_column_int64(statement, 3) + 1;
        merged_json = lbdb_string_duplicate(lbdb_statement_column_text(statement, 4));
        source_changed = lbdb_statement_column_int64(statement, 5) != 0;
        tags_changed = lbdb_statement_column_int64(statement, 6) != 0;
        if (merged_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate revised question");
        }
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = parse_question_data(app, database, merged_json, &question);
    }
    if (error == LBDB_OK) {
        error = sections_for_range(app, database, unit_id, question.source_line_start,
                                   question.source_line_end, &ranges);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "UPDATE question_bank SET earliest_checkpoint=?1,question_type=?2,response_format=?3,"
            "prompt=?4,options_json=?5,expected_answer=?6,grading_criteria_json=?7,"
            "answer_justification=?8,source_section=?9,source_pages=?10,source_line_start=?11,"
            "source_line_end=?12,revision=?13,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
            "WHERE id=?14",
            &statement);
    }
    int bind = 1;
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_double(
            statement, bind++,
            checkpoint_for_range(unit_start, unit_end, question.source_line_end));
    }
#define REVISE_TEXT(value)                                                                         \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(statement, bind++, (value));                          \
        }                                                                                          \
    } while (false)
#define REVISE_INT(value)                                                                          \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(statement, bind++, (value));                         \
        }                                                                                          \
    } while (false)
    REVISE_TEXT(question.question_type);
    REVISE_TEXT(question.response_format);
    REVISE_TEXT(question.prompt);
    REVISE_TEXT(question.options_json);
    REVISE_TEXT(question.expected_answer);
    REVISE_TEXT(question.criteria_json);
    REVISE_TEXT(question.answer_justification);
    REVISE_TEXT(question.source_section);
    REVISE_TEXT(question.source_pages);
    REVISE_INT(question.source_line_start);
    REVISE_INT(question.source_line_end);
    REVISE_INT(revision);
    REVISE_INT(question_id);
#undef REVISE_TEXT
#undef REVISE_INT
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, NULL, "Cannot revise bank question");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK && source_changed) {
        error = lbdb_statement_prepare(
            database, "DELETE FROM question_sources WHERE question_id=?1", &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = step_statement(app, database, statement, NULL, "Cannot replace source links");
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
        if (error == LBDB_OK) {
            error = insert_source_links(app, database, "question", question_id, unit_id, input_json,
                                        question.source_line_start, question.source_line_end,
                                        question.answer_justification);
        }
    }
    if (error == LBDB_OK && tags_changed) {
        error = lbdb_json_get_raw(app, database, input_json, "$.tags", true, &tags_json);
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database, "DELETE FROM question_tags WHERE question_id=?1", &statement);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = step_statement(app, database, statement, NULL, "Cannot replace question tags");
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
        if (error == LBDB_OK) {
            error = insert_question_tags(app, database, question_id, tags_json);
        }
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "question_id") || !lbdb_json_int(details, question_id) ||
            !lbdb_json_key(details, "revision") || !lbdb_json_int(details, revision) ||
            !lbdb_json_key(details, "reason") || !lbdb_json_string(details, reason) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build revision audit");
        } else {
            error = lbdb_commit_write(app, database, "bank.revise", "bank_question", question_id,
                                      lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "bank.revise");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "question_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, question_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "revision"));
        LBDB_JSON(app, lbdb_json_int(app->output, revision));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    section_ranges_destroy(&ranges);
    question_data_destroy(&question);
    free(tags_json);
    free(input_json);
    free(reason);
    free(merged_json);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError bank_set_active(LbdbCommand *command, bool active) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *reason = NULL;
    int64_t question_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (!active && error == LBDB_OK) {
        error = lbdb_args_option(&args, "--reason", true, &reason);
    }
    if (error == LBDB_OK) {
        error = one_positive_positional(command, &args, &question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && !active && !nonempty_text(reason)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Retirement reason is required");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database, "SELECT active FROM question_bank WHERE id=?1",
                                       &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, &has_row, "Cannot load bank question");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown bank question: %lld",
                              (long long)question_id);
    }
    if (error == LBDB_OK && (lbdb_statement_column_int64(statement, 0) != 0) == active) {
        error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Bank question %lld is already %s",
                              (long long)question_id, active ? "active" : "retired");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        const char *sql = active ? "UPDATE question_bank SET active=1,metadata_json=json_remove("
                                   "metadata_json,'$.retirement_reason'),updated_at=strftime("
                                   "'%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1"
                                 : "UPDATE question_bank SET active=0,metadata_json=json_set("
                                   "metadata_json,'$.retirement_reason',?2),updated_at=strftime("
                                   "'%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1";
        error = lbdb_statement_prepare(database, sql, &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK && !active) {
        error = lbdb_statement_bind_text(statement, 2, reason);
    }
    if (error == LBDB_OK) {
        error = step_statement(app, database, statement, NULL,
                               active ? "Cannot activate question" : "Cannot retire question");
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "question_id") || !lbdb_json_int(details, question_id) ||
            !lbdb_json_key(details, "active") || !lbdb_json_bool(details, active) ||
            (!active &&
             (!lbdb_json_key(details, "reason") || !lbdb_json_string(details, reason))) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build bank audit record");
        } else {
            error = lbdb_commit_write(app, database, active ? "bank.activate" : "bank.retire",
                                      "bank_question", question_id, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, active ? "bank.activate" : "bank.retire");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "question_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, question_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "active"));
        LBDB_JSON(app, lbdb_json_bool(app->output, active));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_bank_retire(LbdbCommand *command) { return bank_set_active(command, false); }

LbdbError lbdb_command_bank_activate(LbdbCommand *command) {
    return bank_set_active(command, true);
}
