#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct LbdbManifestUnit {
    char *corpus_slug;
    char *unit_key;
    char *unit_type;
    char *source_path;
    char *absolute_path;
    char *included_reason;
    char *coverage_exempt_sections;
    int64_t position;
    bool include_in_quizzes;
} LbdbManifestUnit;

typedef struct LbdbManifestUnits {
    LbdbManifestUnit *items;
    size_t count;
    size_t capacity;
} LbdbManifestUnits;

typedef struct LbdbSourceSection {
    char *key;
    char *title;
    int64_t position;
    int64_t start_page;
    int64_t end_page;
    int64_t start_line;
    int64_t end_line;
    bool is_summary;
} LbdbSourceSection;

typedef struct LbdbSourceDocument {
    char *title;
    int64_t line_count;
    int64_t start_page;
    int64_t end_page;
    LbdbSourceSection *sections;
    size_t section_count;
    size_t section_capacity;
} LbdbSourceDocument;

static void manifest_units_destroy(LbdbManifestUnits *units) {
    for (size_t index = 0; index < units->count; ++index) {
        free(units->items[index].corpus_slug);
        free(units->items[index].unit_key);
        free(units->items[index].unit_type);
        free(units->items[index].source_path);
        free(units->items[index].absolute_path);
        free(units->items[index].included_reason);
        free(units->items[index].coverage_exempt_sections);
    }
    free(units->items);
    *units = (LbdbManifestUnits){0};
}

static void source_document_destroy(LbdbSourceDocument *document) {
    free(document->title);
    for (size_t index = 0; index < document->section_count; ++index) {
        free(document->sections[index].key);
        free(document->sections[index].title);
    }
    free(document->sections);
    *document = (LbdbSourceDocument){0};
}

static bool text_present(const char *value) {
    if (value == NULL) {
        return false;
    }
    while (*value != '\0') {
        if (!isspace((unsigned char)*value)) {
            return true;
        }
        ++value;
    }
    return false;
}

static LbdbError push_manifest_unit(LbdbApp *app, LbdbManifestUnits *units,
                                    const LbdbManifestUnit *unit) {
    LbdbManifestUnit *replacement = NULL;
    size_t capacity = 0U;
    if (units->count == units->capacity) {
        capacity = units->capacity == 0U ? 16U : units->capacity * 2U;
        if (capacity < units->capacity || capacity > SIZE_MAX / sizeof(*replacement)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Manifest has too many units");
        }
        replacement = realloc(units->items, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate manifest units");
        }
        units->items = replacement;
        units->capacity = capacity;
    }
    units->items[units->count++] = *unit;
    return LBDB_OK;
}

static LbdbError check_manifest_shape(LbdbApp *app, LbdbDatabase *database, const char *json) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    bool version_present = false;
    int64_t format_version = 0;
    LbdbError error = lbdb_json_document_type(app, database, json, "object");
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.format_version", true, &format_version,
                                  &version_present);
    }
    if (error == LBDB_OK && format_version != 1) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_UNSUPPORTED, "Unsupported manifest format version: %lld",
                          (long long)format_version);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT json_type(?1,'$.corpora'),json_array_length(?1,'$.corpora'),"
            "coalesce(sum(json_type(value,'$.slug')='text' AND "
            "json_type(value,'$.units')='array' AND json_array_length(value,'$.units')>0),0),"
            "count(*) FROM json_each(?1,'$.corpora')",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, json);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
    }
    if (error != LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot validate corpus manifest");
    } else if (!has_row || lbdb_statement_column_is_null(statement, 0) ||
               strcmp(lbdb_statement_column_text(statement, 0), "array") != 0 ||
               lbdb_statement_column_int64(statement, 1) <= 0 ||
               lbdb_statement_column_int64(statement, 2) !=
                   lbdb_statement_column_int64(statement, 3)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Manifest needs a non-empty corpora array; each corpus needs a slug "
                              "and non-empty units array");
    }
    (void)version_present;
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError load_manifest(LbdbApp *app, LbdbDatabase *database, LbdbManifestUnits *units) {
    char *json = NULL;
    size_t json_size = 0U;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_read_file(app, app->manifest_path, &json, &json_size);
    (void)json_size;
    if (error == LBDB_OK) {
        error = check_manifest_shape(app, database, json);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT json_extract(c.value,'$.slug'),CAST(u.key AS INTEGER)+1,"
            "json_extract(u.value,'$.key'),json_extract(u.value,'$.type'),"
            "json_extract(u.value,'$.path'),json_type(u.value,'$.include_in_quizzes'),"
            "json_extract(u.value,'$.included_reason'),"
            "coalesce(json_extract(u.value,'$.coverage_exempt_sections'),'[]'),"
            "CASE WHEN json_type(u.value,'$.coverage_exempt_sections') IS NULL THEN 1 "
            "WHEN json_type(u.value,'$.coverage_exempt_sections')='array' AND NOT EXISTS("
            "SELECT 1 FROM json_each(u.value,'$.coverage_exempt_sections') e "
            "WHERE e.type<>'text' OR trim(e.value)='') AND json_array_length("
            "u.value,'$.coverage_exempt_sections')=(SELECT count(DISTINCT e.value) "
            "FROM json_each(u.value,'$.coverage_exempt_sections') e) THEN 1 ELSE 0 END "
            "FROM json_each(?1,'$.corpora') c JOIN json_each(c.value,'$.units') u "
            "ORDER BY CAST(c.key AS INTEGER),CAST(u.key AS INTEGER)",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, json);
    }
    while (error == LBDB_OK) {
        LbdbManifestUnit unit = {0};
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const char *include_type = lbdb_statement_column_text(statement, 5);
        const char *corpus_slug = lbdb_statement_column_text(statement, 0);
        const char *unit_key = lbdb_statement_column_text(statement, 2);
        const char *unit_type = lbdb_statement_column_text(statement, 3);
        const char *source_path = lbdb_statement_column_text(statement, 4);
        const char *included_reason = lbdb_statement_column_text(statement, 6);
        const char *coverage_exempt_sections = lbdb_statement_column_text(statement, 7);
        if (lbdb_statement_column_int64(statement, 8) != 1) {
            error = lbdb_app_fail(
                app, LBDB_ERROR_VALIDATION,
                "coverage_exempt_sections must be an array of unique non-empty strings");
            break;
        }
        if (!text_present(corpus_slug) || !text_present(unit_key) || !text_present(unit_type) ||
            !text_present(source_path) || !text_present(included_reason) || include_type == NULL ||
            (strcmp(include_type, "true") != 0 && strcmp(include_type, "false") != 0)) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Every manifest unit requires key, type, relative path, boolean "
                                  "include_in_quizzes, and included_reason");
            break;
        }
        if (strcmp(unit_type, "chapter") != 0 && strcmp(unit_type, "appendix") != 0) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Invalid unit type: %s", unit_type);
            break;
        }
        if (source_path[0] == '/') {
            error =
                lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Manifest source paths must be project-relative: %s", source_path);
            break;
        }
        unit.corpus_slug = lbdb_string_duplicate(corpus_slug);
        unit.position = lbdb_statement_column_int64(statement, 1);
        unit.unit_key = lbdb_string_duplicate(unit_key);
        unit.unit_type = lbdb_string_duplicate(unit_type);
        unit.source_path = lbdb_string_duplicate(source_path);
        unit.included_reason = lbdb_string_duplicate(included_reason);
        unit.coverage_exempt_sections = lbdb_string_duplicate(coverage_exempt_sections);
        unit.include_in_quizzes = strcmp(include_type, "true") == 0;
        if (unit.corpus_slug == NULL || unit.unit_key == NULL || unit.unit_type == NULL ||
            unit.source_path == NULL || unit.included_reason == NULL ||
            unit.coverage_exempt_sections == NULL) {
            free(unit.corpus_slug);
            free(unit.unit_key);
            free(unit.unit_type);
            free(unit.source_path);
            free(unit.included_reason);
            free(unit.coverage_exempt_sections);
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate manifest unit");
            break;
        }
        error = lbdb_resolve_path(app, unit.source_path, false, true, &unit.absolute_path);
        if (error != LBDB_OK) {
            free(unit.corpus_slug);
            free(unit.unit_key);
            free(unit.unit_type);
            free(unit.source_path);
            free(unit.included_reason);
            free(unit.coverage_exempt_sections);
            break;
        }
        for (size_t index = 0; index < units->count && error == LBDB_OK; ++index) {
            if (strcmp(units->items[index].corpus_slug, unit.corpus_slug) == 0 &&
                strcmp(units->items[index].unit_key, unit.unit_key) == 0) {
                error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Duplicate manifest unit: %s/%s",
                                      unit.corpus_slug, unit.unit_key);
            } else if (strcmp(units->items[index].source_path, unit.source_path) == 0) {
                error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                      "Duplicate manifest source path: %s", unit.source_path);
            }
        }
        if (error == LBDB_OK) {
            error = push_manifest_unit(app, units, &unit);
        }
        if (error != LBDB_OK) {
            free(unit.corpus_slug);
            free(unit.unit_key);
            free(unit.unit_type);
            free(unit.source_path);
            free(unit.absolute_path);
            free(unit.included_reason);
            free(unit.coverage_exempt_sections);
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot parse corpus manifest");
    }
    if (error == LBDB_OK && units->count == 0U) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Manifest declares no units");
    }
    lbdb_statement_destroy(statement);
    free(json);
    return error;
}

static bool parse_page_marker(const char *line, int64_t *page) {
    char *end = NULL;
    long long parsed = 0;
    if (strncmp(line, "## Page ", 8U) != 0) {
        return false;
    }
    parsed = strtoll(line + 8, &end, 10);
    if (end == line + 8 || *end != '\0' || parsed <= 0) {
        return false;
    }
    *page = (int64_t)parsed;
    return true;
}

static bool is_numbered_heading(const char *title) {
    const unsigned char *cursor = (const unsigned char *)title;
    if (isdigit(*cursor)) {
        while (isdigit(*cursor)) {
            ++cursor;
        }
    } else if (isupper(*cursor)) {
        ++cursor;
    } else {
        return false;
    }
    if (*cursor != '.') {
        return false;
    }
    while (*cursor == '.') {
        ++cursor;
        if (!isdigit(*cursor)) {
            return false;
        }
        while (isdigit(*cursor)) {
            ++cursor;
        }
    }
    return *cursor == ' ' && cursor[1] != '\0';
}

static char *heading_key(const char *title) {
    if (strcasecmp(title, "Summary") == 0) {
        return lbdb_string_duplicate("summary");
    }
    const char *space = strchr(title, ' ');
    if (space == NULL) {
        return NULL;
    }
    const size_t size = (size_t)(space - title);
    char *key = malloc(size + 1U);
    if (key == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < size; ++index) {
        key[index] = (char)tolower((unsigned char)title[index]);
    }
    key[size] = '\0';
    return key;
}

static LbdbError push_section(LbdbApp *app, LbdbSourceDocument *document,
                              LbdbSourceSection *section) {
    LbdbSourceSection *replacement = NULL;
    size_t capacity = 0U;
    if (document->section_count == document->section_capacity) {
        capacity = document->section_capacity == 0U ? 8U : document->section_capacity * 2U;
        if (capacity < document->section_capacity || capacity > SIZE_MAX / sizeof(*replacement)) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Source has too many sections");
            return LBDB_ERROR_MEMORY;
        }
        replacement = realloc(document->sections, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source sections");
            return LBDB_ERROR_MEMORY;
        }
        document->sections = replacement;
        document->section_capacity = capacity;
    }
    document->sections[document->section_count++] = *section;
    return LBDB_OK;
}

static LbdbError parse_source_document(LbdbApp *app, const char *path,
                                       LbdbSourceDocument *document) {
    char *contents = NULL;
    size_t content_size = 0U;
    LbdbStringVector lines = {0};
    int64_t *pages = NULL;
    int64_t current_page = 0;
    int64_t first_marker_line = 0;
    LbdbError error = lbdb_read_file(app, path, &contents, &content_size);
    if (error != LBDB_OK) {
        return error;
    }
    char *line_start = contents;
    for (size_t index = 0; index <= content_size; ++index) {
        if (index != content_size && contents[index] != '\n') {
            continue;
        }
        if (index == content_size && line_start == contents + content_size) {
            break;
        }
        contents[index] = '\0';
        size_t line_size = strlen(line_start);
        if (line_size > 0U && line_start[line_size - 1U] == '\r') {
            line_start[line_size - 1U] = '\0';
        }
        error = lbdb_string_vector_push(&lines, line_start);
        if (error != LBDB_OK) {
            error = lbdb_app_fail(app, error, "Unable to store source lines");
            break;
        }
        line_start = contents + index + 1U;
    }
    free(contents);
    if (error != LBDB_OK) {
        lbdb_string_vector_destroy(&lines);
        return error;
    }
    if (lines.count == 0U || strncmp(lines.items[0], "# ", 2U) != 0 ||
        !text_present(lines.items[0] + 2)) {
        lbdb_string_vector_destroy(&lines);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Source needs a '# ' document title: %s",
                             path);
    }
    if (lines.count > (size_t)INT64_MAX - 1U) {
        lbdb_string_vector_destroy(&lines);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Source has too many lines: %s", path);
    }
    document->title = lbdb_string_duplicate(lines.items[0] + 2);
    document->line_count = (int64_t)lines.count;
    pages = calloc(lines.count + 1U, sizeof(*pages));
    if (document->title == NULL || pages == NULL) {
        free(pages);
        lbdb_string_vector_destroy(&lines);
        source_document_destroy(document);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source index");
    }
    for (size_t index = 0; index < lines.count; ++index) {
        int64_t marker = 0;
        if (parse_page_marker(lines.items[index], &marker)) {
            current_page = marker;
            if (first_marker_line == 0) {
                first_marker_line = (int64_t)index + 1;
                document->start_page = marker;
            }
            document->end_page = marker;
        }
        pages[index + 1U] = current_page;
    }
    if (first_marker_line == 0) {
        free(pages);
        lbdb_string_vector_destroy(&lines);
        source_document_destroy(document);
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Source has no '## Page N' markers: %s",
                             path);
    }
    LbdbSourceSection opening = {.key = lbdb_string_duplicate("opening"),
                                 .title = lbdb_string_duplicate("Opening"),
                                 .position = 1,
                                 .start_page = pages[(size_t)first_marker_line],
                                 .end_page = document->end_page,
                                 .start_line = first_marker_line,
                                 .end_line = document->line_count,
                                 .is_summary = false};
    if (opening.key == NULL || opening.title == NULL) {
        free(opening.key);
        free(opening.title);
        (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate opening section");
        error = LBDB_ERROR_MEMORY;
    } else {
        error = push_section(app, document, &opening);
    }
    for (size_t index = 1U; error == LBDB_OK && index < lines.count; ++index) {
        if ((int64_t)index + 1 <= first_marker_line) {
            continue;
        }
        const char *line = lines.items[index];
        size_t hashes = 0U;
        while (line[hashes] == '#' && hashes < 6U) {
            ++hashes;
        }
        if (hashes < 2U || hashes > 6U || line[hashes] != ' ') {
            continue;
        }
        const char *title = line + hashes + 1U;
        int64_t ignored_page = 0;
        if (parse_page_marker(line, &ignored_page) ||
            (strcasecmp(title, "Summary") != 0 && !is_numbered_heading(title))) {
            continue;
        }
        LbdbSourceSection *previous = &document->sections[document->section_count - 1U];
        previous->end_line = (int64_t)index;
        previous->end_page = pages[index];
        LbdbSourceSection section = {
            .key = heading_key(title),
            .title = lbdb_string_duplicate(title),
            .position = (int64_t)document->section_count + 1,
            .start_page = pages[index + 1U],
            .end_page = document->end_page,
            .start_line = (int64_t)index + 1,
            .end_line = document->line_count,
            .is_summary = strcasecmp(title, "Summary") == 0,
        };
        if (section.key == NULL || section.title == NULL) {
            free(section.key);
            free(section.title);
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source section");
            break;
        }
        bool duplicate = false;
        for (size_t existing = 0; existing < document->section_count; ++existing) {
            if (strcmp(document->sections[existing].key, section.key) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            char *unique = lbdb_string_format("%s-%lld", section.key, (long long)section.position);
            free(section.key);
            section.key = unique;
            if (unique == NULL) {
                free(section.title);
                error =
                    lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate unique section key");
                break;
            }
        }
        error = push_section(app, document, &section);
    }
    if (error == LBDB_OK) {
        LbdbSourceSection *last = &document->sections[document->section_count - 1U];
        last->end_line = document->line_count;
        last->end_page = pages[lines.count];
    }
    free(pages);
    lbdb_string_vector_destroy(&lines);
    if (error != LBDB_OK) {
        source_document_destroy(document);
    }
    return error;
}

static LbdbError bind_and_step(LbdbApp *app, LbdbDatabase *database, LbdbStatement *statement) {
    LbdbError error = lbdb_statement_step(statement, NULL);
    return error == LBDB_OK
               ? LBDB_OK
               : lbdb_app_database_error(app, database, error, "Cannot synchronize corpus row");
}

static LbdbError insert_sections(LbdbApp *app, LbdbDatabase *database, int64_t unit_id,
                                 const LbdbSourceDocument *document) {
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_statement_prepare(
        database,
        "INSERT INTO source_sections(unit_id,section_key,title,position,start_page,end_page,"
        "start_line,end_line,is_summary,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'{}')",
        &statement);
    for (size_t index = 0; error == LBDB_OK && index < document->section_count; ++index) {
        const LbdbSourceSection *section = &document->sections[index];
        error = lbdb_statement_bind_int64(statement, 1, unit_id);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, section->key);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 3, section->title);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 4, section->position);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 5, section->start_page);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 6, section->end_page);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 7, section->start_line);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 8, section->end_line);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 9, section->is_summary ? 1 : 0);
        }
        if (error == LBDB_OK) {
            error = bind_and_step(app, database, statement);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_reset(statement);
        }
    }
    if (error != LBDB_OK && app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot insert source sections");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError apply_section_exemptions(LbdbApp *app, LbdbDatabase *database,
                                          const LbdbManifestUnit *unit, int64_t unit_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT e.value FROM json_each(?1) e WHERE NOT EXISTS(SELECT 1 FROM source_sections s "
        "WHERE s.unit_id=?2 AND s.section_key=e.value) ORDER BY CAST(e.key AS INTEGER) LIMIT 1",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, unit->coverage_exempt_sections);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, unit_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
    }
    if (error == LBDB_OK && has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Unknown coverage-exempt section: %s/%s/%s", unit->corpus_slug,
                              unit->unit_key, lbdb_statement_column_text(statement, 0));
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "UPDATE source_sections SET metadata_json=CASE WHEN EXISTS(SELECT 1 FROM "
            "json_each(?1) e WHERE e.value=source_sections.section_key) THEN "
            "json_set(metadata_json,'$.coverage_exempt',json('true')) ELSE "
            "json_remove(metadata_json,'$.coverage_exempt') END WHERE unit_id=?2",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, unit->coverage_exempt_sections);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, unit_id);
    }
    if (error == LBDB_OK) {
        error = bind_and_step(app, database, statement);
    }
    if (error != LBDB_OK && app->error == LBDB_OK) {
        error = lbdb_app_database_error(app, database, error, "Cannot apply section exemptions");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError synchronize_unit(LbdbApp *app, LbdbDatabase *database,
                                  const LbdbManifestUnit *manifest_unit,
                                  const LbdbSourceDocument *document, const char *digest,
                                  int64_t *unit_id, bool *created) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t existing_id = 0;
    char *existing_hash = NULL;
    int64_t bank_rows = 0;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id,content_sha256,(SELECT count(*) FROM concepts WHERE unit_id=source_units.id)+"
        "(SELECT count(*) FROM question_bank WHERE unit_id=source_units.id) "
        "FROM source_units WHERE corpus_slug=?1 AND unit_key=?2",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, manifest_unit->corpus_slug);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, manifest_unit->unit_key);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
    }
    if (error == LBDB_OK && has_row) {
        existing_id = lbdb_statement_column_int64(statement, 0);
        existing_hash = lbdb_string_duplicate(lbdb_statement_column_text(statement, 1));
        bank_rows = lbdb_statement_column_int64(statement, 2);
        if (existing_hash == NULL) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate source hash");
            error = LBDB_ERROR_MEMORY;
        }
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error != LBDB_OK) {
        free(existing_hash);
        return app->error == LBDB_OK
                   ? lbdb_app_database_error(app, database, error, "Cannot inspect source unit")
                   : error;
    }
    if (has_row && bank_rows > 0 && strcmp(existing_hash, digest) != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT,
                              "Source changed after bank questions were imported: %s",
                              manifest_unit->source_path);
        free(existing_hash);
        return error;
    }
    if (has_row) {
        error = lbdb_statement_prepare(
            database,
            "UPDATE source_units SET unit_type=?1,title=?2,source_path=?3,position=?4,"
            "start_page=?5,end_page=?6,start_line=1,end_line=?7,include_in_quizzes=?8,"
            "included_reason=?9,content_sha256=?10 WHERE id=?11",
            &statement);
    } else {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO source_units(corpus_slug,unit_key,unit_type,title,source_path,position,"
            "start_page,end_page,start_line,end_line,include_in_quizzes,included_reason,"
            "content_sha256,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1,?9,?10,?11,?12,'{}')",
            &statement);
    }
    int bind_index = 1;
#define BIND_TEXT(value)                                                                           \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(statement, bind_index++, (value));                    \
        }                                                                                          \
    } while (false)
#define BIND_INT(value)                                                                            \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(statement, bind_index++, (value));                   \
        }                                                                                          \
    } while (false)
    if (!has_row) {
        BIND_TEXT(manifest_unit->corpus_slug);
        BIND_TEXT(manifest_unit->unit_key);
    }
    BIND_TEXT(manifest_unit->unit_type);
    BIND_TEXT(document->title);
    BIND_TEXT(manifest_unit->source_path);
    BIND_INT(manifest_unit->position);
    BIND_INT(document->start_page);
    BIND_INT(document->end_page);
    BIND_INT(document->line_count);
    BIND_INT(manifest_unit->include_in_quizzes ? 1 : 0);
    BIND_TEXT(manifest_unit->included_reason);
    BIND_TEXT(digest);
    if (has_row) {
        BIND_INT(existing_id);
    }
#undef BIND_TEXT
#undef BIND_INT
    if (error == LBDB_OK) {
        error = bind_and_step(app, database, statement);
    }
    if (error == LBDB_OK) {
        *unit_id = has_row ? existing_id : lbdb_statement_last_insert_id(statement);
        *created = !has_row;
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK && has_row && bank_rows == 0 && strcmp(existing_hash, digest) != 0) {
        error = lbdb_statement_prepare(database, "DELETE FROM source_sections WHERE unit_id=?1",
                                       &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, existing_id);
        }
        if (error == LBDB_OK) {
            error = bind_and_step(app, database, statement);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "SELECT count(*) FROM source_sections WHERE unit_id=?1", &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, *unit_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_step(statement, &has_row);
        }
        const int64_t section_count =
            error == LBDB_OK && has_row ? lbdb_statement_column_int64(statement, 0) : 0;
        lbdb_statement_destroy(statement);
        statement = NULL;
        if (error == LBDB_OK && section_count == 0) {
            error = insert_sections(app, database, *unit_id, document);
        }
    }
    if (error == LBDB_OK) {
        error = apply_section_exemptions(app, database, manifest_unit, *unit_id);
    }
    free(existing_hash);
    return error;
}

LbdbError lbdb_command_corpus_sync(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbManifestUnits units = {0};
    LbdbJsonWriter *unit_output = NULL;
    size_t created_count = 0U;
    LbdbError error = LBDB_OK;
    LbdbArgs args = {0};

    error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    lbdb_args_destroy(&args);
    if (error != LBDB_OK) {
        return error;
    }
    error = lbdb_begin_write(app, &database);
    if (error == LBDB_OK) {
        error = load_manifest(app, database, &units);
    }
    unit_output = lbdb_json_writer_create(app->pretty);
    if (error == LBDB_OK && (unit_output == NULL || !lbdb_json_begin_array(unit_output))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate corpus output");
    }
    for (size_t index = 0; error == LBDB_OK && index < units.count; ++index) {
        LbdbSourceDocument document = {0};
        char digest[65] = {0};
        int64_t unit_id = 0;
        bool created = false;
        char *canonical = NULL;
        error = lbdb_resolve_path(app, units.items[index].source_path, true, true, &canonical);
        if (error == LBDB_OK) {
            free(units.items[index].absolute_path);
            units.items[index].absolute_path = canonical;
            error = parse_source_document(app, canonical, &document);
        }
        if (error == LBDB_OK) {
            error = lbdb_file_sha256(app, canonical, digest);
        }
        if (error == LBDB_OK) {
            error = synchronize_unit(app, database, &units.items[index], &document, digest,
                                     &unit_id, &created);
        }
        if (error == LBDB_OK) {
            if (created) {
                created_count += 1U;
            }
            if (!lbdb_json_begin_object(unit_output) || !lbdb_json_key(unit_output, "id") ||
                !lbdb_json_int(unit_output, unit_id) ||
                !lbdb_json_key(unit_output, "corpus_slug") ||
                !lbdb_json_string(unit_output, units.items[index].corpus_slug) ||
                !lbdb_json_key(unit_output, "unit_key") ||
                !lbdb_json_string(unit_output, units.items[index].unit_key) ||
                !lbdb_json_key(unit_output, "action") ||
                !lbdb_json_string(unit_output, created ? "created" : "updated") ||
                !lbdb_json_end_object(unit_output)) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build corpus output");
            }
        }
        source_document_destroy(&document);
    }
    if (error == LBDB_OK && !lbdb_json_end_array(unit_output)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize corpus output");
    }
    if (error == LBDB_OK) {
        LbdbStatement *statement = NULL;
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO metadata(key,value) VALUES('indexed_source_count',?1) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            &statement);
        if (error == LBDB_OK) {
            char count_text[32] = {0};
            const int length = snprintf(count_text, sizeof(count_text), "%zu", units.count);
            if (length <= 0 || (size_t)length >= sizeof(count_text)) {
                error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format source count");
            } else {
                error = lbdb_statement_bind_text(statement, 1, count_text);
            }
        }
        if (error == LBDB_OK) {
            error = bind_and_step(app, database, statement);
        }
        lbdb_statement_destroy(statement);
    }
    if (error == LBDB_OK) {
        char details[128] = {0};
        const int length = snprintf(details, sizeof(details),
                                    "{\"declared_units\":%zu,\"created\":%zu,\"updated\":%zu}",
                                    units.count, created_count, units.count - created_count);
        if (length <= 0 || (size_t)length >= sizeof(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format corpus audit details");
        } else {
            error = lbdb_commit_write(app, database, "corpus.sync", "corpus", 0, details);
        }
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "corpus.sync");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "manifest"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->manifest_path));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, (int64_t)units.count));
        LBDB_JSON(app, lbdb_json_key(app->output, "units"));
        LBDB_JSON(app, lbdb_json_raw(app->output, lbdb_json_data(unit_output)));
        error = lbdb_output_end(app);
    }
    lbdb_json_writer_destroy(unit_output);
    manifest_units_destroy(&units);
    lbdb_database_close(database);
    return error;
}

LbdbError lbdb_command_corpus_status(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbDatabase *database = NULL;
    LbdbManifestUnits units = {0};
    LbdbStatement *statement = NULL;
    bool all_in_sync = true;
    LbdbError error = LBDB_OK;
    LbdbArgs args = {0};

    error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    lbdb_args_destroy(&args);
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = load_manifest(app, database, &units);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "corpus.status");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "manifest"));
        LBDB_JSON(app, lbdb_json_string(app->output, app->manifest_path));
        LBDB_JSON(app, lbdb_json_key(app->output, "units"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    for (size_t index = 0; error == LBDB_OK && index < units.count; ++index) {
        bool has_row = false;
        bool source_exists = lbdb_path_is_file(units.items[index].absolute_path);
        char digest[65] = {0};
        if (source_exists) {
            char *canonical = NULL;
            error = lbdb_resolve_path(app, units.items[index].source_path, true, true, &canonical);
            if (error == LBDB_OK) {
                free(units.items[index].absolute_path);
                units.items[index].absolute_path = canonical;
                error = lbdb_file_sha256(app, canonical, digest);
            }
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database,
                "SELECT id,content_sha256,include_in_quizzes,source_path,NOT EXISTS(SELECT 1 "
                "FROM source_sections s WHERE s.unit_id=source_units.id AND "
                "((coalesce(json_extract(s.metadata_json,'$.coverage_exempt'),0)=1) <> "
                "EXISTS(SELECT 1 FROM json_each(?3) e WHERE e.value=s.section_key))) AND "
                "NOT EXISTS(SELECT 1 FROM json_each(?3) e WHERE NOT EXISTS(SELECT 1 FROM "
                "source_sections s WHERE s.unit_id=source_units.id AND s.section_key=e.value)) "
                "AS coverage_exemptions_match FROM source_units "
                "WHERE corpus_slug=?1 AND unit_key=?2",
                &statement);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, units.items[index].corpus_slug);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, units.items[index].unit_key);
        }
        if (error == LBDB_OK) {
            error =
                lbdb_statement_bind_text(statement, 3, units.items[index].coverage_exempt_sections);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_step(statement, &has_row);
        }
        if (error != LBDB_OK) {
            error = lbdb_app_database_error(app, database, error, "Cannot inspect corpus status");
        }
        const bool hash_matches = error == LBDB_OK && has_row && source_exists &&
                                  strcmp(lbdb_statement_column_text(statement, 1), digest) == 0;
        const bool coverage_exemptions_match =
            error == LBDB_OK && has_row && lbdb_statement_column_int64(statement, 4) == 1;
        const bool in_sync =
            error == LBDB_OK && has_row && hash_matches &&
            lbdb_statement_column_int64(statement, 2) ==
                (units.items[index].include_in_quizzes ? 1 : 0) &&
            strcmp(lbdb_statement_column_text(statement, 3), units.items[index].source_path) == 0 &&
            coverage_exemptions_match;
        if (!in_sync) {
            all_in_sync = false;
        }
        if (error == LBDB_OK) {
            LBDB_JSON(app, lbdb_json_begin_object(app->output));
            LBDB_JSON(app, lbdb_json_key(app->output, "corpus_slug"));
            LBDB_JSON(app, lbdb_json_string(app->output, units.items[index].corpus_slug));
            LBDB_JSON(app, lbdb_json_key(app->output, "unit_key"));
            LBDB_JSON(app, lbdb_json_string(app->output, units.items[index].unit_key));
            LBDB_JSON(app, lbdb_json_key(app->output, "database_id"));
            if (has_row) {
                LBDB_JSON(app,
                          lbdb_json_int(app->output, lbdb_statement_column_int64(statement, 0)));
            } else {
                LBDB_JSON(app, lbdb_json_null(app->output));
            }
            LBDB_JSON(app, lbdb_json_key(app->output, "source_exists"));
            LBDB_JSON(app, lbdb_json_bool(app->output, source_exists));
            LBDB_JSON(app, lbdb_json_key(app->output, "hash_matches"));
            LBDB_JSON(app, lbdb_json_bool(app->output, hash_matches));
            LBDB_JSON(app, lbdb_json_key(app->output, "coverage_exemptions_match"));
            LBDB_JSON(app, lbdb_json_bool(app->output, coverage_exemptions_match));
            LBDB_JSON(app, lbdb_json_key(app->output, "in_sync"));
            LBDB_JSON(app, lbdb_json_bool(app->output, in_sync));
            LBDB_JSON(app, lbdb_json_end_object(app->output));
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    int64_t unlisted = 0;
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        error = lbdb_statement_prepare(
            database,
            "SELECT count(*) FROM source_units su WHERE NOT EXISTS("
            "SELECT 1 FROM json_each(?1,'$.corpora') c JOIN json_each(c.value,'$.units') u "
            "WHERE json_extract(c.value,'$.slug')=su.corpus_slug "
            "AND json_extract(u.value,'$.key')=su.unit_key)",
            &statement);
        char *manifest_json = NULL;
        size_t manifest_size = 0U;
        if (error == LBDB_OK) {
            error = lbdb_read_file(app, app->manifest_path, &manifest_json, &manifest_size);
        }
        (void)manifest_size;
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, manifest_json);
        }
        bool has_row = false;
        if (error == LBDB_OK) {
            error = lbdb_statement_step(statement, &has_row);
        }
        if (error == LBDB_OK && has_row) {
            unlisted = lbdb_statement_column_int64(statement, 0);
        }
        free(manifest_json);
        lbdb_statement_destroy(statement);
        statement = NULL;
        if (unlisted > 0) {
            all_in_sync = false;
        }
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "unlisted_database_units"));
        LBDB_JSON(app, lbdb_json_int(app->output, unlisted));
        LBDB_JSON(app, lbdb_json_key(app->output, "in_sync"));
        LBDB_JSON(app, lbdb_json_bool(app->output, all_in_sync));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    manifest_units_destroy(&units);
    lbdb_database_close(database);
    return error;
}
