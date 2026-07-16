#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct LbdbTransition {
    const char *key;
} LbdbTransition;

typedef struct LbdbIntVector {
    int64_t *items;
    size_t count;
    size_t capacity;
} LbdbIntVector;

typedef struct LbdbFollowUp {
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
    int64_t objective_id;
    int64_t unit_id;
} LbdbFollowUp;

static const LbdbTransition session_transitions[] = {
    {"abandon|in_progress|abandoned"},  {"abandon|paused|abandoned"}, {"abandon|planned|abandoned"},
    {"complete|in_progress|completed"}, {"pause|in_progress|paused"}, {"resume|paused|in_progress"},
    {"start|planned|in_progress"},
};

static const LbdbTransition question_transitions[] = {
    {"answer|answered|answered"}, {"answer|asked|answered"}, {"defer|asked|deferred"},
    {"next|planned|asked"},       {"requeue|asked|planned"}, {"requeue|deferred|planned"},
};

static const char *const session_states[] = {"abandoned", "completed", "in_progress", "paused",
                                             "planned"};
static const char *const quiz_scopes[] = {"chapter_checkpoint", "chapter_final", "theme", "topic"};
static const char *const assessment_values[] = {"correct", "incorrect", "partially_correct"};
static const char *const follow_up_question_types[] = {
    "application", "code_reading",  "code_writing", "debugging",
    "example",     "misconception", "recall",       "relationship",
};
static const char *const follow_up_formats[] = {"code_response", "free_response",
                                                "multiple_choice"};

static bool has_text(const char *value) {
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

static LbdbError push_int(LbdbApp *app, LbdbIntVector *vector, int64_t value) {
    int64_t *replacement = NULL;
    size_t capacity = 0U;
    if (vector->count == vector->capacity) {
        capacity = vector->capacity == 0U ? 16U : vector->capacity * 2U;
        if (capacity < vector->capacity || capacity > SIZE_MAX / sizeof(*replacement)) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Quiz selection is too large");
            return LBDB_ERROR_MEMORY;
        }
        replacement = realloc(vector->items, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            (void)lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate quiz selection");
            return LBDB_ERROR_MEMORY;
        }
        vector->items = replacement;
        vector->capacity = capacity;
    }
    vector->items[vector->count++] = value;
    return LBDB_OK;
}

static void follow_up_destroy(LbdbFollowUp *follow_up) {
    free(follow_up->question_type);
    free(follow_up->response_format);
    free(follow_up->prompt);
    free(follow_up->options_json);
    free(follow_up->expected_answer);
    free(follow_up->criteria_json);
    free(follow_up->answer_justification);
    free(follow_up->source_section);
    free(follow_up->source_pages);
    *follow_up = (LbdbFollowUp){0};
}

static bool transition_exists(const LbdbTransition *transitions, size_t count, const char *key) {
    size_t lower = 0U;
    size_t upper = count;
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2U;
        const int comparison = strcmp(key, transitions[middle].key);
        if (comparison == 0) {
            return true;
        }
        if (comparison < 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return false;
}

static LbdbError require_transition(LbdbApp *app, bool session, const char *event, const char *from,
                                    const char *to, int64_t entity_id) {
    char *key = lbdb_string_format("%s|%s|%s", event, from, to);
    if (key == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate transition key");
    }
    const bool allowed =
        session
            ? transition_exists(session_transitions,
                                sizeof(session_transitions) / sizeof(session_transitions[0]), key)
            : transition_exists(question_transitions,
                                sizeof(question_transitions) / sizeof(question_transitions[0]),
                                key);
    free(key);
    return allowed ? LBDB_OK
                   : lbdb_app_fail(app, LBDB_ERROR_STATE,
                                   "%s %lld cannot transition from %s to %s through %s",
                                   session ? "Quiz" : "Question", (long long)entity_id, from, to,
                                   event);
}

static LbdbError database_error(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                                const char *operation) {
    return error == LBDB_OK ? LBDB_OK : lbdb_app_database_error(app, database, error, operation);
}

static LbdbError one_quiz_id(LbdbCommand *command, LbdbArgs *args, int64_t *quiz_id) {
    LbdbStringVector values = {0};
    LbdbError error = lbdb_args_positionals(args, &values);
    if (error == LBDB_OK && values.count != 1U) {
        error = lbdb_app_fail(command->app, LBDB_ERROR_USAGE, "%s requires exactly one quiz ID",
                              command->key);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(command->app, "quiz ID", values.items[0], quiz_id);
    }
    lbdb_string_vector_destroy(&values);
    return error;
}

static LbdbError load_session(LbdbApp *app, LbdbDatabase *database, int64_t quiz_id,
                              LbdbStatement **statement) {
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id,created_at,completed_at,scope_type,source_unit_id,scope_label,state,"
        "coverage_policy,delivery,template_id,base_question_count FROM quiz_sessions WHERE id=?1",
        statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(*statement, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(*statement, &has_row),
                               "Cannot load quiz session");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz: %lld", (long long)quiz_id);
    }
    return error;
}

static LbdbError write_progress(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                int64_t quiz_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT total_questions,base_questions,follow_up_questions,planned_questions,"
        "asked_questions,deferred_questions,answered_questions,retired_questions,"
        "correct_responses,partially_correct_responses,incorrect_responses "
        "FROM quiz_progress WHERE quiz_id=?1",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                               "Cannot load quiz progress");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz: %lld", (long long)quiz_id);
    }
    static const char *const names[] = {
        "total_questions",     "base_questions",
        "follow_up_questions", "planned_questions",
        "asked_questions",     "deferred_questions",
        "answered_questions",  "retired_questions",
        "correct_responses",   "partially_correct_responses",
        "incorrect_responses",
    };
    if (error == LBDB_OK && !lbdb_json_begin_object(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz progress");
    }
    for (int column = 0; error == LBDB_OK && column < 11; ++column) {
        if (!lbdb_json_key(writer, names[column]) ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, column))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz progress");
        }
    }
    if (error == LBDB_OK && !lbdb_json_end_object(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize quiz progress");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError write_session_summary(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                       LbdbStatement *session) {
    const int64_t quiz_id = lbdb_statement_column_int64(session, 0);
    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
        !lbdb_json_int(writer, quiz_id) || !lbdb_json_key(writer, "created_at") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 1)) ||
        !lbdb_json_key(writer, "completed_at") ||
        !lbdb_json_string_or_null(writer, lbdb_statement_column_text(session, 2)) ||
        !lbdb_json_key(writer, "scope_type") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 3)) ||
        !lbdb_json_key(writer, "source_unit_id")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
    }
    if (lbdb_statement_column_is_null(session, 4)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
        }
    } else if (!lbdb_json_int(writer, lbdb_statement_column_int64(session, 4))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
    }
    if (!lbdb_json_key(writer, "scope_label") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 5)) ||
        !lbdb_json_key(writer, "state") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 6)) ||
        !lbdb_json_key(writer, "coverage_policy") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 7)) ||
        !lbdb_json_key(writer, "delivery") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(session, 8)) ||
        !lbdb_json_key(writer, "template_id")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
    }
    if (lbdb_statement_column_is_null(session, 9)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
        }
    } else if (!lbdb_json_int(writer, lbdb_statement_column_int64(session, 9))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
    }
    if (!lbdb_json_key(writer, "base_question_count") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(session, 10)) ||
        !lbdb_json_key(writer, "progress")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz summary");
    }
    LbdbError error = write_progress(app, database, writer, quiz_id);
    if (error == LBDB_OK && !lbdb_json_end_object(writer)) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize quiz summary");
    }
    return error;
}

static LbdbError write_public_question(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                       int64_t question_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id,quiz_id,objective_id,position,origin,question_type,response_format,prompt,"
        "options_json,source_section,source_pages,state,asked_at FROM quiz_questions WHERE id=?1",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                               "Cannot load quiz question");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz question: %lld",
                              (long long)question_id);
    }
    if (error == LBDB_OK && (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
                             !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
                             !lbdb_json_key(writer, "quiz_id") ||
                             !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 1)) ||
                             !lbdb_json_key(writer, "objective_id") ||
                             !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 2)) ||
                             !lbdb_json_key(writer, "position") ||
                             !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 3)) ||
                             !lbdb_json_key(writer, "origin") ||
                             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 4)) ||
                             !lbdb_json_key(writer, "question_type") ||
                             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 5)) ||
                             !lbdb_json_key(writer, "response_format") ||
                             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 6)) ||
                             !lbdb_json_key(writer, "prompt") ||
                             !lbdb_json_string(writer, lbdb_statement_column_text(statement, 7)) ||
                             !lbdb_json_key(writer, "options"))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz question");
    }
    if (error == LBDB_OK) {
        if (lbdb_statement_column_is_null(statement, 8)) {
            if (!lbdb_json_null(writer)) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz question");
            }
        } else if (!lbdb_json_raw(writer, lbdb_statement_column_text(statement, 8))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz question");
        }
    }
    if (error == LBDB_OK &&
        (!lbdb_json_key(writer, "source_section") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 9)) ||
         !lbdb_json_key(writer, "source_pages") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 10)) ||
         !lbdb_json_key(writer, "state") ||
         !lbdb_json_string(writer, lbdb_statement_column_text(statement, 11)) ||
         !lbdb_json_key(writer, "asked_at") ||
         !lbdb_json_string_or_null(writer, lbdb_statement_column_text(statement, 12)) ||
         !lbdb_json_end_object(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize quiz question");
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError select_template_questions(LbdbApp *app, LbdbDatabase *database,
                                           int64_t template_id, bool adaptive, int64_t limit,
                                           LbdbIntVector *questions) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    const char *sql =
        adaptive
            ? "SELECT q.id FROM quiz_template_questions l JOIN question_bank q ON "
              "q.id=l.question_id "
              "WHERE l.template_id=?1 AND q.active=1 ORDER BY coalesce((SELECT CASE r.assessment "
              "WHEN 'incorrect' THEN 1 WHEN 'partially_correct' THEN 2 WHEN 'correct' THEN 3 END "
              "FROM quiz_responses r JOIN quiz_questions snapshot ON snapshot.id=r.question_id "
              "WHERE snapshot.bank_question_id=q.id ORDER BY r.answered_at DESC,r.id DESC LIMIT "
              "1),0),"
              "l.position LIMIT ?2"
            : "SELECT q.id FROM quiz_template_questions l JOIN question_bank q ON "
              "q.id=l.question_id "
              "WHERE l.template_id=?1 AND q.active=1 ORDER BY l.position";
    LbdbError error = lbdb_statement_prepare(database, sql, &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, template_id);
    }
    if (error == LBDB_OK && adaptive) {
        error = lbdb_statement_bind_int64(statement, 2, limit);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = push_int(app, questions, lbdb_statement_column_int64(statement, 0));
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot select template questions");
    }
    lbdb_statement_destroy(statement);
    if (error == LBDB_OK && questions->count == 0U) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Quiz template has no active questions");
    }
    return error;
}

static int64_t objective_for_concept(const LbdbIntVector *concepts, const LbdbIntVector *objectives,
                                     int64_t concept_id) {
    for (size_t index = 0; index < concepts->count; ++index) {
        if (concepts->items[index] == concept_id) {
            return objectives->items[index];
        }
    }
    return 0;
}

static LbdbError create_quiz_snapshot(LbdbApp *app, LbdbDatabase *database,
                                      LbdbStatement *template_statement, int64_t effective_limit,
                                      int64_t *quiz_id, int64_t *base_count) {
    const int64_t template_id = lbdb_statement_column_int64(template_statement, 0);
    const char *template_scope = lbdb_statement_column_text(template_statement, 1);
    const char *title = lbdb_statement_column_text(template_statement, 2);
    const bool adaptive =
        strcmp(template_scope, "topic") == 0 || strcmp(template_scope, "theme") == 0;
    LbdbIntVector questions = {0};
    LbdbIntVector concepts = {0};
    LbdbIntVector objectives = {0};
    LbdbStatement *statement = NULL;
    LbdbError error = select_template_questions(app, database, template_id, adaptive,
                                                effective_limit, &questions);
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_sessions(scope_type,source_unit_id,scope_label,state,"
            "coverage_policy,delivery,template_id,base_question_count,metadata_json) "
            "VALUES(?1,?2,?3,'planned','balanced_weighted','cli',?4,?5,"
            "json_object('selection_policy',?6,'dynamic_followups',json('true')))",
            &statement);
    }
    const char *scope = strcmp(template_scope, "checkpoint") == 0 ? "chapter_checkpoint"
                        : strcmp(template_scope, "final") == 0    ? "chapter_final"
                                                                  : template_scope;
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_column_is_null(template_statement, 3)
                    ? lbdb_statement_bind_null(statement, 2)
                    : lbdb_statement_bind_int64(statement, 2,
                                                lbdb_statement_column_int64(template_statement, 3));
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 3, title);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 4, template_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 5, (int64_t)questions.count);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 6,
                                         lbdb_statement_column_text(template_statement, 4));
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, NULL),
                               "Cannot create quiz session");
    }
    if (error == LBDB_OK) {
        *quiz_id = lbdb_statement_last_insert_id(statement);
        *base_count = (int64_t)questions.count;
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        char payload[128] = {0};
        const int length =
            snprintf(payload, sizeof(payload), "{\"template_id\":%lld,\"base_questions\":%lld}",
                     (long long)template_id, (long long)*base_count);
        if (length <= 0 || (size_t)length >= sizeof(payload)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format planned quiz event");
        } else {
            error = lbdb_add_quiz_event(app, database, *quiz_id, 0, 0, "session_planned", NULL,
                                        payload);
        }
    }
    for (size_t index = 0; error == LBDB_OK && index < questions.count; ++index) {
        bool has_row = false;
        error = lbdb_statement_prepare(
            database,
            "SELECT q.concept_id,c.name,c.importance,c.source_pages,c.source_line_start,"
            "c.source_line_end,c.justification,s.title FROM question_bank q "
            "JOIN concepts c ON c.id=q.concept_id JOIN source_sections s ON "
            "s.id=c.primary_section_id WHERE q.id=?1",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, questions.items[index]);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot load quiz concept");
        }
        if (error == LBDB_OK && !has_row) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Template question has no source concept");
        }
        const int64_t concept_id = error == LBDB_OK ? lbdb_statement_column_int64(statement, 0) : 0;
        if (error == LBDB_OK && objective_for_concept(&concepts, &objectives, concept_id) == 0) {
            LbdbStatement *insert = NULL;
            error = lbdb_statement_prepare(
                database,
                "INSERT INTO quiz_objectives(quiz_id,position,section_title,concept,importance,"
                "source_pages,source_line_start,source_line_end,justification,concept_id) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
                &insert);
            int bind = 1;
#define OBJECTIVE_INT(value)                                                                       \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(insert, bind++, (value));                            \
        }                                                                                          \
    } while (false)
#define OBJECTIVE_TEXT(value)                                                                      \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(insert, bind++, (value));                             \
        }                                                                                          \
    } while (false)
            OBJECTIVE_INT(*quiz_id);
            OBJECTIVE_INT((int64_t)concepts.count + 1);
            OBJECTIVE_TEXT(lbdb_statement_column_text(statement, 7));
            OBJECTIVE_TEXT(lbdb_statement_column_text(statement, 1));
            OBJECTIVE_TEXT(lbdb_statement_column_text(statement, 2));
            OBJECTIVE_TEXT(lbdb_statement_column_text(statement, 3));
            OBJECTIVE_INT(lbdb_statement_column_int64(statement, 4));
            OBJECTIVE_INT(lbdb_statement_column_int64(statement, 5));
            OBJECTIVE_TEXT(lbdb_statement_column_text(statement, 6));
            OBJECTIVE_INT(concept_id);
#undef OBJECTIVE_INT
#undef OBJECTIVE_TEXT
            if (error == LBDB_OK) {
                error = database_error(app, database, lbdb_statement_step(insert, NULL),
                                       "Cannot create quiz objective");
            }
            const int64_t objective_id =
                error == LBDB_OK ? lbdb_statement_last_insert_id(insert) : 0;
            if (error == LBDB_OK) {
                error = push_int(app, &concepts, concept_id);
            }
            if (error == LBDB_OK) {
                error = push_int(app, &objectives, objective_id);
            }
            lbdb_statement_destroy(insert);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    for (size_t index = 0; error == LBDB_OK && index < questions.count; ++index) {
        bool has_row = false;
        error = lbdb_statement_prepare(
            database,
            "SELECT concept_id,question_type,response_format,prompt,options_json,expected_answer,"
            "grading_criteria_json,answer_justification,source_section,source_pages,"
            "source_line_start,source_line_end FROM question_bank WHERE id=?1",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, questions.items[index]);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot load bank question snapshot");
        }
        const int64_t concept_id =
            error == LBDB_OK && has_row ? lbdb_statement_column_int64(statement, 0) : 0;
        const int64_t objective_id = objective_for_concept(&concepts, &objectives, concept_id);
        LbdbStatement *insert = NULL;
        if (error == LBDB_OK && objective_id == 0) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Snapshot objective is missing");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database,
                "INSERT INTO quiz_questions(quiz_id,objective_id,position,origin,question_type,"
                "response_format,prompt,options_json,expected_answer,grading_criteria_json,"
                "answer_justification,source_section,source_pages,source_line_start,"
                "source_line_end,state,bank_question_id) VALUES(?1,?2,?3,'base',?4,?5,?6,?7,"
                "?8,?9,?10,?11,?12,?13,?14,'planned',?15)",
                &insert);
        }
        int bind = 1;
#define SNAP_INT(value)                                                                            \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(insert, bind++, (value));                            \
        }                                                                                          \
    } while (false)
#define SNAP_TEXT(value)                                                                           \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(insert, bind++, (value));                             \
        }                                                                                          \
    } while (false)
        SNAP_INT(*quiz_id);
        SNAP_INT(objective_id);
        SNAP_INT((int64_t)index + 1);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 1) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 2) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 3) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 4) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 5) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 6) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 7) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 8) : NULL);
        SNAP_TEXT(error == LBDB_OK ? lbdb_statement_column_text(statement, 9) : NULL);
        SNAP_INT(error == LBDB_OK ? lbdb_statement_column_int64(statement, 10) : 0);
        SNAP_INT(error == LBDB_OK ? lbdb_statement_column_int64(statement, 11) : 0);
        SNAP_INT(questions.items[index]);
#undef SNAP_INT
#undef SNAP_TEXT
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(insert, NULL),
                                   "Cannot create question snapshot");
        }
        lbdb_statement_destroy(insert);
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = require_transition(app, true, "start", "planned", "in_progress", *quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "UPDATE quiz_sessions SET state='in_progress' WHERE id=?1 AND state='planned'",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, *quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, NULL),
                               "Cannot start planned quiz session");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        char payload[192] = {0};
        const int length = snprintf(payload, sizeof(payload),
                                    "{\"from\":\"planned\",\"to\":\"in_progress\","
                                    "\"template_id\":%lld,\"base_questions\":%lld}",
                                    (long long)template_id, (long long)*base_count);
        if (length <= 0 || (size_t)length >= sizeof(payload)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format quiz event");
        } else {
            error = lbdb_add_quiz_event(app, database, *quiz_id, 0, 0, "session_started", NULL,
                                        payload);
        }
    }
    free(questions.items);
    free(concepts.items);
    free(objectives.items);
    lbdb_statement_destroy(statement);
    return error;
}

LbdbError lbdb_command_quiz_start(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *template_text = NULL;
    const char *limit_text = NULL;
    int64_t template_id = 0;
    int64_t requested_limit = 0;
    int64_t effective_limit = 0;
    int64_t quiz_id = 0;
    int64_t base_count = 0;
    bool created = false;
    LbdbDatabase *database = NULL;
    LbdbStatement *template_statement = NULL;
    LbdbStatement *existing = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--template", true, &template_text);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--limit", false, &limit_text);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(app, "--template", template_text, &template_id);
    }
    if (error == LBDB_OK && limit_text != NULL) {
        error = lbdb_parse_positive_int(app, "--limit", limit_text, &requested_limit);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,scope_type,title,unit_id,selection_policy FROM quiz_templates "
            "WHERE id=?1 AND active=1",
            &template_statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(template_statement, 1, template_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(template_statement, &has_row),
                               "Cannot load quiz template");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown active template: %lld",
                              (long long)template_id);
    }
    const char *scope = error == LBDB_OK ? lbdb_statement_column_text(template_statement, 1) : NULL;
    const bool chapter =
        scope != NULL && (strcmp(scope, "checkpoint") == 0 || strcmp(scope, "final") == 0);
    if (error == LBDB_OK && chapter && requested_limit > 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Chapter checkpoint and final sessions cannot use --limit");
    }
    if (error == LBDB_OK) {
        effective_limit = chapter ? 0 : requested_limit > 0 ? requested_limit : 10;
        error = lbdb_statement_prepare(
            database,
            "SELECT id,created_at,completed_at,scope_type,source_unit_id,scope_label,state,"
            "coverage_policy,delivery,template_id,base_question_count FROM quiz_sessions "
            "WHERE template_id=?1 AND state IN('planned','in_progress','paused') "
            "ORDER BY id LIMIT 1",
            &existing);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(existing, 1, template_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(existing, &has_row),
                               "Cannot inspect active quiz sessions");
    }
    if (error == LBDB_OK && has_row) {
        quiz_id = lbdb_statement_column_int64(existing, 0);
        base_count = lbdb_statement_column_int64(existing, 10);
    } else if (error == LBDB_OK) {
        error = create_quiz_snapshot(app, database, template_statement, effective_limit, &quiz_id,
                                     &base_count);
        created = error == LBDB_OK;
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "template_id") || !lbdb_json_int(details, template_id) ||
            !lbdb_json_key(details, "created") || !lbdb_json_bool(details, created) ||
            !lbdb_json_key(details, "base_questions") || !lbdb_json_int(details, base_count) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build quiz start audit");
        } else {
            error = lbdb_commit_write(app, database, "quiz.start", "quiz_session", quiz_id,
                                      lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    lbdb_statement_destroy(existing);
    existing = NULL;
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &existing);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "quiz.start");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "created"));
        LBDB_JSON(app, lbdb_json_bool(app->output, created));
        LBDB_JSON(app, lbdb_json_key(app->output, "session"));
        error = write_session_summary(app, database, app->output, existing);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(existing);
    lbdb_statement_destroy(template_statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_quiz_list(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *state = NULL;
    const char *scope = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t count = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--status", false, &state);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--scope", false, &scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && state != NULL &&
        !lbdb_string_in_set(state, session_states,
                            sizeof(session_states) / sizeof(session_states[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown quiz state: %s", state);
    }
    if (error == LBDB_OK && scope != NULL &&
        !lbdb_string_in_set(scope, quiz_scopes, sizeof(quiz_scopes) / sizeof(quiz_scopes[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown quiz scope: %s", scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,created_at,completed_at,scope_type,source_unit_id,scope_label,state,"
            "coverage_policy,delivery,template_id,base_question_count FROM quiz_sessions "
            "WHERE (?1 IS NULL OR state=?1) AND (?2 IS NULL OR scope_type=?2) ORDER BY id DESC",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, state);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "quiz.list");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "sessions"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = write_session_summary(app, database, app->output, statement);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list quiz sessions");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_quiz_status(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    int64_t quiz_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *current = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = one_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT id FROM quiz_questions WHERE quiz_id=?1 "
                                       "AND state='asked' ORDER BY position LIMIT 1",
                                       &current);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(current, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(current, &has_row),
                               "Cannot inspect current question");
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "quiz.status");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "session"));
        error = write_session_summary(app, database, app->output, session);
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "current_question"));
        error =
            has_row
                ? write_public_question(app, database, app->output,
                                        lbdb_statement_column_int64(current, 0))
                : (lbdb_json_null(app->output)
                       ? LBDB_OK
                       : lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build current question"));
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(current);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_quiz_next(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    int64_t quiz_id = 0;
    int64_t question_id = 0;
    bool already_asked = false;
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *question = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = one_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK && strcmp(lbdb_statement_column_text(session, 6), "in_progress") != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_STATE, "Quiz %lld is %s; next requires in_progress",
                              (long long)quiz_id, lbdb_statement_column_text(session, 6));
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(database,
                                       "SELECT id FROM quiz_questions WHERE quiz_id=?1 "
                                       "AND state='asked' ORDER BY position LIMIT 1",
                                       &question);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(question, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(question, &has_row),
                               "Cannot inspect asked question");
    }
    if (error == LBDB_OK && has_row) {
        question_id = lbdb_statement_column_int64(question, 0);
        already_asked = true;
    }
    lbdb_statement_destroy(question);
    question = NULL;
    if (error == LBDB_OK && !has_row) {
        error = lbdb_statement_prepare(database,
                                       "SELECT id,state FROM quiz_questions WHERE quiz_id=?1 "
                                       "AND state='planned' ORDER BY position LIMIT 1",
                                       &question);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(question, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(question, &has_row),
                                   "Cannot select next question");
        }
        if (error == LBDB_OK && !has_row) {
            error = lbdb_app_fail(app, LBDB_ERROR_STATE, "Quiz %lld has no planned questions",
                                  (long long)quiz_id);
        }
        if (error == LBDB_OK) {
            question_id = lbdb_statement_column_int64(question, 0);
            error = require_transition(app, false, "next", lbdb_statement_column_text(question, 1),
                                       "asked", question_id);
        }
        lbdb_statement_destroy(question);
        question = NULL;
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database,
                "UPDATE quiz_questions SET state='asked',asked_at=strftime("
                "'%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?1 AND state='planned'",
                &question);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(question, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(question, NULL),
                                   "Cannot mark next question asked");
        }
        if (error == LBDB_OK) {
            error = lbdb_add_quiz_event(app, database, quiz_id, question_id, 0, "question_asked",
                                        NULL, "{}");
        }
    }
    if (error == LBDB_OK) {
        char details[128] = {0};
        const int length =
            snprintf(details, sizeof(details),
                     "{\"quiz_id\":%lld,\"question_id\":%lld,"
                     "\"already_asked\":%s}",
                     (long long)quiz_id, (long long)question_id, already_asked ? "true" : "false");
        if (length <= 0 || (size_t)length >= sizeof(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format quiz next audit");
        } else {
            error = lbdb_commit_write(app, database, "quiz.next", "quiz_session", quiz_id, details);
        }
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "quiz.next");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "already_asked"));
        LBDB_JSON(app, lbdb_json_bool(app->output, already_asked));
        LBDB_JSON(app, lbdb_json_key(app->output, "question"));
        error = write_public_question(app, database, app->output, question_id);
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "progress"));
        error = write_progress(app, database, app->output, quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(question);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError question_state_change(LbdbCommand *command, bool defer) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *question_text = NULL;
    const char *reason = NULL;
    int64_t quiz_id = 0;
    int64_t question_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *question = NULL;
    bool has_row = false;
    const char *event = defer ? "defer" : "requeue";
    const char *target = defer ? "deferred" : "planned";
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--question", true, &question_text);
    }
    if (defer && error == LBDB_OK) {
        error = lbdb_args_option(&args, "--reason", true, &reason);
    }
    if (error == LBDB_OK) {
        error = one_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(app, "--question", question_text, &question_id);
    }
    if (error == LBDB_OK && defer && !has_text(reason)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Deferral reason is required");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK && strcmp(lbdb_statement_column_text(session, 6), "in_progress") != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_STATE,
                              "Quiz %lld is %s; question changes require in_progress",
                              (long long)quiz_id, lbdb_statement_column_text(session, 6));
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "SELECT state FROM quiz_questions WHERE id=?1 AND quiz_id=?2", &question);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(question, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(question, 2, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(question, &has_row),
                               "Cannot load quiz question");
    }
    if (error == LBDB_OK && !has_row) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Question %lld does not belong to quiz %lld",
                          (long long)question_id, (long long)quiz_id);
    }
    char *previous = NULL;
    if (error == LBDB_OK) {
        previous = lbdb_string_duplicate(lbdb_statement_column_text(question, 0));
        if (previous == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate question state");
        }
    }
    if (error == LBDB_OK) {
        error = require_transition(app, false, event, previous, target, question_id);
    }
    lbdb_statement_destroy(question);
    question = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            defer ? "UPDATE quiz_questions SET state='deferred' WHERE id=?1"
                  : "UPDATE quiz_questions SET state='planned',asked_at=NULL WHERE id=?1",
            &question);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(question, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(question, NULL),
                               defer ? "Cannot defer question" : "Cannot requeue question");
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *payload = lbdb_json_writer_create(false);
        if (payload == NULL || !lbdb_json_begin_object(payload) ||
            !lbdb_json_key(payload, "previous_state") || !lbdb_json_string(payload, previous) ||
            !lbdb_json_end_object(payload)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question event");
        } else {
            error = lbdb_add_quiz_event(app, database, quiz_id, question_id, 0,
                                        defer ? "question_deferred" : "question_requeued", reason,
                                        lbdb_json_data(payload));
        }
        lbdb_json_writer_destroy(payload);
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "quiz_id") || !lbdb_json_int(details, quiz_id) ||
            !lbdb_json_key(details, "question_id") || !lbdb_json_int(details, question_id) ||
            !lbdb_json_key(details, "previous_state") || !lbdb_json_string(details, previous) ||
            !lbdb_json_key(details, "state") || !lbdb_json_string(details, target) ||
            (defer && (!lbdb_json_key(details, "reason") || !lbdb_json_string(details, reason))) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build question audit");
        } else {
            error = lbdb_commit_write(app, database, defer ? "quiz.defer" : "quiz.requeue",
                                      "quiz_question", question_id, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, defer ? "quiz.defer" : "quiz.requeue");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "quiz_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, quiz_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "question_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, question_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "state"));
        LBDB_JSON(app, lbdb_json_string(app->output, target));
        LBDB_JSON(app, lbdb_json_key(app->output, "progress"));
        error = write_progress(app, database, app->output, quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    free(previous);
    lbdb_statement_destroy(question);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_quiz_defer(LbdbCommand *command) {
    return question_state_change(command, true);
}

LbdbError lbdb_command_quiz_requeue(LbdbCommand *command) {
    return question_state_change(command, false);
}

static LbdbError read_json_argument(LbdbApp *app, const char *argument, char **json) {
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

static LbdbError validate_text_array(LbdbApp *app, LbdbDatabase *database, const char *json,
                                     size_t minimum, size_t maximum, const char *field) {
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
        error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                               "Cannot validate follow-up array");
    }
    if (error == LBDB_OK) {
        const int64_t count = lbdb_statement_column_int64(statement, 0);
        if (!has_row || count < (int64_t)minimum || count > (int64_t)maximum ||
            count != lbdb_statement_column_int64(statement, 1)) {
            error =
                lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "%s must contain %zu-%zu non-empty strings", field, minimum, maximum);
        }
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError parse_follow_up(LbdbApp *app, LbdbDatabase *database, const char *json,
                                 int64_t quiz_id, LbdbFollowUp *follow_up) {
    bool present = false;
    int64_t objective_id = 0;
    int64_t objective_position = 0;
    int64_t concept_id = 0;
    int objective_links = 0;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_json_document_type(app, database, json, "object");
    if (error == LBDB_OK) {
        error = lbdb_json_get_text(app, database, json, "$.question_type", true,
                                   &follow_up->question_type);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_text(app, database, json, "$.response_format", true,
                                   &follow_up->response_format);
    }
#define FOLLOW_TEXT(path, target)                                                                  \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_json_get_text(app, database, json, (path), true, &(target));              \
        }                                                                                          \
    } while (false)
    FOLLOW_TEXT("$.prompt", follow_up->prompt);
    FOLLOW_TEXT("$.expected_answer", follow_up->expected_answer);
    FOLLOW_TEXT("$.answer_justification", follow_up->answer_justification);
    FOLLOW_TEXT("$.source_section", follow_up->source_section);
    FOLLOW_TEXT("$.source_pages", follow_up->source_pages);
#undef FOLLOW_TEXT
    if (error == LBDB_OK) {
        error =
            lbdb_json_get_raw(app, database, json, "$.options", false, &follow_up->options_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_raw(app, database, json, "$.grading_criteria", true,
                                  &follow_up->criteria_json);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.source_line_start", true,
                                  &follow_up->source_line_start, &present);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.source_line_end", true,
                                  &follow_up->source_line_end, &present);
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.objective_id", false, &objective_id,
                                  &present);
        objective_links += present ? 1 : 0;
    }
    if (error == LBDB_OK) {
        error = lbdb_json_get_int(app, database, json, "$.objective_position", false,
                                  &objective_position, &present);
        objective_links += present ? 1 : 0;
    }
    if (error == LBDB_OK) {
        error =
            lbdb_json_get_int(app, database, json, "$.concept_id", false, &concept_id, &present);
        objective_links += present ? 1 : 0;
    }
    if (error == LBDB_OK && objective_links != 1) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Follow-up requires exactly one of objective_id, "
                              "objective_position, or concept_id");
    }
    if (error == LBDB_OK && !lbdb_string_in_set(follow_up->question_type, follow_up_question_types,
                                                sizeof(follow_up_question_types) /
                                                    sizeof(follow_up_question_types[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unknown question type: %s",
                              follow_up->question_type);
    }
    if (error == LBDB_OK &&
        !lbdb_string_in_set(follow_up->response_format, follow_up_formats,
                            sizeof(follow_up_formats) / sizeof(follow_up_formats[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Unknown response format: %s",
                              follow_up->response_format);
    }
    if (error == LBDB_OK &&
        (!has_text(follow_up->prompt) || !has_text(follow_up->expected_answer) ||
         !has_text(follow_up->answer_justification) || !has_text(follow_up->source_section) ||
         !has_text(follow_up->source_pages) || follow_up->source_line_start <= 0 ||
         follow_up->source_line_end < follow_up->source_line_start)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Follow-up text and ordered positive source lines are required");
    }
    if (error == LBDB_OK) {
        error = validate_text_array(app, database, follow_up->criteria_json, 1U, (size_t)INT64_MAX,
                                    "grading_criteria");
    }
    if (error == LBDB_OK && strcmp(follow_up->response_format, "multiple_choice") == 0) {
        if (follow_up->options_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Multiple-choice follow-ups require options");
        } else {
            error = validate_text_array(app, database, follow_up->options_json, 3U, 5U, "options");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_prepare(
                database, "SELECT count(*) FROM json_each(?1) WHERE value=?2", &statement);
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 1, follow_up->options_json);
            }
            if (error == LBDB_OK) {
                error = lbdb_statement_bind_text(statement, 2, follow_up->expected_answer);
            }
            if (error == LBDB_OK) {
                error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                       "Cannot validate follow-up answer");
            }
            if (error == LBDB_OK && lbdb_statement_column_int64(statement, 0) != 1) {
                error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                      "Expected answer must match exactly one option");
            }
            lbdb_statement_destroy(statement);
            statement = NULL;
        }
    } else if (error == LBDB_OK && follow_up->options_json != NULL) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Only multiple-choice follow-ups may contain options");
    }
    if (error == LBDB_OK) {
        const char *sql = objective_id > 0
                              ? "SELECT o.id,c.unit_id FROM quiz_objectives o JOIN concepts c ON "
                                "c.id=o.concept_id WHERE o.quiz_id=?1 AND o.id=?2"
                          : objective_position > 0
                              ? "SELECT o.id,c.unit_id FROM quiz_objectives o JOIN concepts c ON "
                                "c.id=o.concept_id WHERE o.quiz_id=?1 AND o.position=?2"
                              : "SELECT o.id,c.unit_id FROM quiz_objectives o JOIN concepts c ON "
                                "c.id=o.concept_id WHERE o.quiz_id=?1 AND o.concept_id=?2";
        const int64_t reference = objective_id > 0         ? objective_id
                                  : objective_position > 0 ? objective_position
                                                           : concept_id;
        error = lbdb_statement_prepare(database, sql, &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 2, reference);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot resolve follow-up objective");
        }
        if (error == LBDB_OK && !has_row) {
            error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND,
                                  "Follow-up objective does not belong to the quiz");
        }
        if (error == LBDB_OK) {
            follow_up->objective_id = lbdb_statement_column_int64(statement, 0);
            follow_up->unit_id = lbdb_statement_column_int64(statement, 1);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT count(*),coalesce(sum(is_summary),0),min(start_line),max(end_line) "
            "FROM source_sections WHERE unit_id=?1 AND start_line<=?2 AND end_line>=?3",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, follow_up->unit_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 2, follow_up->source_line_end);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 3, follow_up->source_line_start);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot validate follow-up source range");
        }
        if (error == LBDB_OK &&
            (lbdb_statement_column_int64(statement, 0) == 0 ||
             lbdb_statement_column_int64(statement, 1) != 0 ||
             lbdb_statement_column_int64(statement, 2) > follow_up->source_line_start ||
             lbdb_statement_column_int64(statement, 3) < follow_up->source_line_end)) {
            error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                                  "Follow-up source range must be covered by body sections");
        }
        lbdb_statement_destroy(statement);
    }
    return error;
}

LbdbError lbdb_command_quiz_follow_up(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *input = NULL;
    char *json = NULL;
    int64_t quiz_id = 0;
    int64_t question_id = 0;
    int64_t position = 0;
    LbdbFollowUp follow_up = {0};
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--input", true, &input);
    }
    if (error == LBDB_OK) {
        error = one_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = read_json_argument(app, input, &json);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK && strcmp(lbdb_statement_column_text(session, 6), "in_progress") != 0) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_STATE, "Quiz %lld is %s; follow-ups require in_progress",
                          (long long)quiz_id, lbdb_statement_column_text(session, 6));
    }
    if (error == LBDB_OK) {
        error = parse_follow_up(app, database, json, quiz_id, &follow_up);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "SELECT coalesce(max(position),0)+1 FROM quiz_questions WHERE quiz_id=?1",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, quiz_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                               "Cannot allocate follow-up position");
    }
    if (error == LBDB_OK) {
        position = lbdb_statement_column_int64(statement, 0);
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_questions(quiz_id,objective_id,position,origin,question_type,"
            "response_format,prompt,options_json,expected_answer,grading_criteria_json,"
            "answer_justification,source_section,source_pages,source_line_start,source_line_end,"
            "state,bank_question_id) VALUES(?1,?2,?3,'follow_up',?4,?5,?6,?7,?8,?9,?10,?11,"
            "?12,?13,?14,'planned',NULL)",
            &statement);
    }
    int bind = 1;
#define FOLLOW_BIND_INT(value)                                                                     \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_int64(statement, bind++, (value));                         \
        }                                                                                          \
    } while (false)
#define FOLLOW_BIND_TEXT(value)                                                                    \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_statement_bind_text(statement, bind++, (value));                          \
        }                                                                                          \
    } while (false)
    FOLLOW_BIND_INT(quiz_id);
    FOLLOW_BIND_INT(follow_up.objective_id);
    FOLLOW_BIND_INT(position);
    FOLLOW_BIND_TEXT(follow_up.question_type);
    FOLLOW_BIND_TEXT(follow_up.response_format);
    FOLLOW_BIND_TEXT(follow_up.prompt);
    FOLLOW_BIND_TEXT(follow_up.options_json);
    FOLLOW_BIND_TEXT(follow_up.expected_answer);
    FOLLOW_BIND_TEXT(follow_up.criteria_json);
    FOLLOW_BIND_TEXT(follow_up.answer_justification);
    FOLLOW_BIND_TEXT(follow_up.source_section);
    FOLLOW_BIND_TEXT(follow_up.source_pages);
    FOLLOW_BIND_INT(follow_up.source_line_start);
    FOLLOW_BIND_INT(follow_up.source_line_end);
#undef FOLLOW_BIND_INT
#undef FOLLOW_BIND_TEXT
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, NULL),
                               "Cannot append follow-up question");
    }
    if (error == LBDB_OK) {
        question_id = lbdb_statement_last_insert_id(statement);
        char payload[128] = {0};
        const int length =
            snprintf(payload, sizeof(payload), "{\"objective_id\":%lld,\"position\":%lld}",
                     (long long)follow_up.objective_id, (long long)position);
        if (length <= 0 || (size_t)length >= sizeof(payload)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format follow-up event");
        } else {
            error = lbdb_add_quiz_event(app, database, quiz_id, question_id, 0, "follow_up_added",
                                        NULL, payload);
        }
    }
    if (error == LBDB_OK) {
        char details[160] = {0};
        const int length = snprintf(details, sizeof(details),
                                    "{\"quiz_id\":%lld,\"question_id\":%lld,\"objective_id\":%lld,"
                                    "\"position\":%lld}",
                                    (long long)quiz_id, (long long)question_id,
                                    (long long)follow_up.objective_id, (long long)position);
        if (length <= 0 || (size_t)length >= sizeof(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format follow-up audit");
        } else {
            error = lbdb_commit_write(app, database, "quiz.follow-up", "quiz_question", question_id,
                                      details);
        }
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "quiz.follow-up");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "quiz_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, quiz_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "question_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, question_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "position"));
        LBDB_JSON(app, lbdb_json_int(app->output, position));
        LBDB_JSON(app, lbdb_json_key(app->output, "progress"));
        error = write_progress(app, database, app->output, quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    follow_up_destroy(&follow_up);
    free(json);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError session_state_change(LbdbCommand *command, const char *event,
                                      const char *target_state, bool optional_reason,
                                      bool required_reason, bool require_resolved) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *reason = NULL;
    int64_t quiz_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *session = NULL;
    LbdbStatement *statement = NULL;
    char *previous_state = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if ((optional_reason || required_reason) && error == LBDB_OK) {
        error = lbdb_args_option(&args, "--reason", required_reason, &reason);
    }
    if (error == LBDB_OK) {
        error = one_quiz_id(command, &args, &quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && required_reason && !has_text(reason)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "A non-empty reason is required");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK) {
        previous_state = lbdb_string_duplicate(lbdb_statement_column_text(session, 6));
        if (previous_state == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate session state");
        }
    }
    if (error == LBDB_OK) {
        error = require_transition(app, true, event, previous_state, target_state, quiz_id);
    }
    if (error == LBDB_OK && require_resolved) {
        error = lbdb_statement_prepare(
            database,
            "SELECT count(*),coalesce(json_group_array(json_object('id',id,'position',position,"
            "'state',state)),'[]') FROM quiz_questions WHERE quiz_id=?1 "
            "AND state IN('planned','asked','deferred')",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot inspect unresolved questions");
        }
        if (error == LBDB_OK && lbdb_statement_column_int64(statement, 0) > 0) {
            error = lbdb_app_fail_details(app, LBDB_ERROR_STATE,
                                          lbdb_statement_column_text(statement, 1),
                                          "Quiz %lld has unresolved questions", (long long)quiz_id);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK && strcmp(event, "abandon") == 0) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_events(quiz_id,question_id,event_type,reason,payload_json) "
            "SELECT quiz_id,id,'question_retired',?2,json_object('previous_state',state) "
            "FROM quiz_questions WHERE quiz_id=?1 AND state IN('planned','asked','deferred') "
            "ORDER BY position",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, reason);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, NULL),
                                   "Cannot record abandoned question retirements");
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK && strcmp(event, "abandon") == 0) {
        error = lbdb_statement_prepare(database,
                                       "UPDATE quiz_questions SET state='retired' WHERE quiz_id=?1 "
                                       "AND state IN('planned','asked','deferred')",
                                       &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, quiz_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, NULL),
                                   "Cannot retire abandoned questions");
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        const bool terminal =
            strcmp(target_state, "completed") == 0 || strcmp(target_state, "abandoned") == 0;
        error = lbdb_statement_prepare(
            database,
            terminal ? "UPDATE quiz_sessions SET state=?1,completed_at=strftime("
                       "'%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?2"
                     : "UPDATE quiz_sessions SET state=?1,completed_at=NULL WHERE id=?2",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, target_state);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 2, quiz_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, NULL),
                                   "Cannot update quiz session state");
        }
    }
    if (error == LBDB_OK) {
        char *event_name = lbdb_string_format("session_%s", event);
        if (event_name == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate session event");
        } else {
            LbdbJsonWriter *payload = lbdb_json_writer_create(false);
            if (payload == NULL || !lbdb_json_begin_object(payload) ||
                !lbdb_json_key(payload, "from") || !lbdb_json_string(payload, previous_state) ||
                !lbdb_json_key(payload, "to") || !lbdb_json_string(payload, target_state) ||
                !lbdb_json_end_object(payload)) {
                error =
                    lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build session event payload");
            } else {
                error = lbdb_add_quiz_event(app, database, quiz_id, 0, 0, event_name, reason,
                                            lbdb_json_data(payload));
            }
            lbdb_json_writer_destroy(payload);
            free(event_name);
        }
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "quiz_id") || !lbdb_json_int(details, quiz_id) ||
            !lbdb_json_key(details, "from") || !lbdb_json_string(details, previous_state) ||
            !lbdb_json_key(details, "to") || !lbdb_json_string(details, target_state) ||
            (reason != NULL &&
             (!lbdb_json_key(details, "reason") || !lbdb_json_string(details, reason))) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build session audit");
        } else {
            char *command_name = lbdb_string_format("quiz.%s", event);
            if (command_name == NULL) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY,
                                      "Unable to allocate session command name");
            } else {
                error = lbdb_commit_write(app, database, command_name, "quiz_session", quiz_id,
                                          lbdb_json_data(details));
            }
            free(command_name);
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    lbdb_statement_destroy(session);
    session = NULL;
    if (error == LBDB_OK) {
        error = load_session(app, database, quiz_id, &session);
    }
    if (error == LBDB_OK) {
        char *command_name = lbdb_string_format("quiz.%s", event);
        if (command_name == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate command output name");
        } else {
            error = lbdb_output_begin(app, command_name);
        }
        free(command_name);
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "session"));
        error = write_session_summary(app, database, app->output, session);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    free(previous_state);
    lbdb_statement_destroy(session);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_quiz_pause(LbdbCommand *command) {
    return session_state_change(command, "pause", "paused", true, false, false);
}

LbdbError lbdb_command_quiz_resume(LbdbCommand *command) {
    return session_state_change(command, "resume", "in_progress", false, false, false);
}

LbdbError lbdb_command_quiz_complete(LbdbCommand *command) {
    return session_state_change(command, "complete", "completed", false, false, true);
}

LbdbError lbdb_command_quiz_abandon(LbdbCommand *command) {
    return session_state_change(command, "abandon", "abandoned", false, true, false);
}

LbdbError lbdb_command_response_submit(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *question_text = NULL;
    const char *answer = NULL;
    const char *assessment = NULL;
    const char *feedback = NULL;
    const char *learning_topic = NULL;
    const char *learning_state = NULL;
    const char *evidence = NULL;
    const char *next_step = NULL;
    bool reveal_answer = false;
    int64_t question_id = 0;
    int64_t quiz_id = 0;
    int64_t attempt = 0;
    int64_t response_id = 0;
    int64_t learning_record_id = 0;
    char *question_state = NULL;
    char *expected_answer = NULL;
    char *criteria_json = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *question = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
#define RESPONSE_OPTION(name, target, required)                                                    \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_args_option(&args, (name), (required), &(target));                        \
        }                                                                                          \
    } while (false)
    RESPONSE_OPTION("--question", question_text, true);
    RESPONSE_OPTION("--answer", answer, true);
    RESPONSE_OPTION("--assessment", assessment, true);
    RESPONSE_OPTION("--feedback", feedback, true);
    RESPONSE_OPTION("--learning-topic", learning_topic, false);
    RESPONSE_OPTION("--learning-status", learning_state, false);
    RESPONSE_OPTION("--evidence", evidence, false);
    RESPONSE_OPTION("--next-step", next_step, false);
#undef RESPONSE_OPTION
    if (error == LBDB_OK) {
        error = lbdb_args_flag(&args, "--reveal-answer", &reveal_answer);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(app, "--question", question_text, &question_id);
    }
    if (error == LBDB_OK &&
        !lbdb_string_in_set(assessment, assessment_values,
                            sizeof(assessment_values) / sizeof(assessment_values[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown assessment: %s", assessment);
    }
    const int linkage_count = (learning_topic != NULL ? 1 : 0) + (learning_state != NULL ? 1 : 0) +
                              (evidence != NULL ? 1 : 0);
    if (error == LBDB_OK && linkage_count != 0 && linkage_count != 3) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Linked learning requires --learning-topic, --learning-status, "
                              "and --evidence together");
    }
    if (error == LBDB_OK &&
        (!has_text(answer) || !has_text(feedback) ||
         (learning_topic != NULL && (!has_text(learning_topic) || !has_text(evidence))))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Answer, feedback, and linked learning text must not be empty");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT q.quiz_id,q.state,q.expected_answer,q.grading_criteria_json,s.state "
            "FROM quiz_questions q JOIN quiz_sessions s ON s.id=q.quiz_id WHERE q.id=?1",
            &question);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(question, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(question, &has_row),
                               "Cannot load quiz question");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz question: %lld",
                              (long long)question_id);
    }
    if (error == LBDB_OK && strcmp(lbdb_statement_column_text(question, 4), "in_progress") != 0) {
        error =
            lbdb_app_fail(app, LBDB_ERROR_STATE, "Quiz %lld is %s; responses require in_progress",
                          (long long)lbdb_statement_column_int64(question, 0),
                          lbdb_statement_column_text(question, 4));
    }
    if (error == LBDB_OK) {
        quiz_id = lbdb_statement_column_int64(question, 0);
        question_state = lbdb_string_duplicate(lbdb_statement_column_text(question, 1));
        expected_answer = lbdb_string_duplicate(lbdb_statement_column_text(question, 2));
        criteria_json = lbdb_string_duplicate(lbdb_statement_column_text(question, 3));
        if (question_state == NULL || expected_answer == NULL || criteria_json == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate response context");
        }
    }
    if (error == LBDB_OK) {
        error = require_transition(app, false, "answer", question_state, "answered", question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT coalesce(max(attempt_number),0)+1 FROM quiz_responses WHERE question_id=?1",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                                   "Cannot allocate response attempt");
        }
        if (error == LBDB_OK) {
            attempt = lbdb_statement_column_int64(statement, 0);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_responses(question_id,attempt_number,answer,assessment,feedback,"
            "metadata_json) VALUES(?1,?2,?3,?4,?5,'{}')",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, question_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, attempt);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 3, answer);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 4, assessment);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 5, feedback);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, NULL),
                               "Cannot store quiz response");
    }
    if (error == LBDB_OK) {
        response_id = lbdb_statement_last_insert_id(statement);
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "UPDATE quiz_questions SET state='answered' WHERE id=?1", &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, question_id);
        }
        if (error == LBDB_OK) {
            error = database_error(app, database, lbdb_statement_step(statement, NULL),
                                   "Cannot mark question answered");
        }
    }
    if (error == LBDB_OK) {
        char payload[160] = {0};
        const int length =
            snprintf(payload, sizeof(payload),
                     "{\"attempt_number\":%lld,\"assessment\":\"%s\",\"previous_state\":\"%s\"}",
                     (long long)attempt, assessment, question_state);
        if (length <= 0 || (size_t)length >= sizeof(payload)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format response event");
        } else {
            error = lbdb_add_quiz_event(app, database, quiz_id, question_id, response_id,
                                        "answer_submitted", NULL, payload);
        }
    }
    if (error == LBDB_OK && learning_topic != NULL) {
        error = lbdb_insert_learning_record(app, database, learning_topic, learning_state, evidence,
                                            next_step, "quiz", quiz_id, response_id,
                                            &learning_record_id);
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "question_id") || !lbdb_json_int(details, question_id) ||
            !lbdb_json_key(details, "quiz_id") || !lbdb_json_int(details, quiz_id) ||
            !lbdb_json_key(details, "attempt_number") || !lbdb_json_int(details, attempt) ||
            !lbdb_json_key(details, "assessment") || !lbdb_json_string(details, assessment) ||
            !lbdb_json_key(details, "learning_record_id") ||
            (learning_record_id > 0 ? !lbdb_json_int(details, learning_record_id)
                                    : !lbdb_json_null(details)) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build response audit");
        } else {
            error = lbdb_commit_write(app, database, "response.submit", "quiz_response",
                                      response_id, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "response.submit");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "response_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, response_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "question_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, question_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "quiz_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, quiz_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "attempt_number"));
        LBDB_JSON(app, lbdb_json_int(app->output, attempt));
        LBDB_JSON(app, lbdb_json_key(app->output, "assessment"));
        LBDB_JSON(app, lbdb_json_string(app->output, assessment));
        LBDB_JSON(app, lbdb_json_key(app->output, "feedback"));
        LBDB_JSON(app, lbdb_json_string(app->output, feedback));
        LBDB_JSON(app, lbdb_json_key(app->output, "learning_record_id"));
        if (learning_record_id > 0) {
            LBDB_JSON(app, lbdb_json_int(app->output, learning_record_id));
        } else {
            LBDB_JSON(app, lbdb_json_null(app->output));
        }
        LBDB_JSON(app, lbdb_json_key(app->output, "progress"));
        error = write_progress(app, database, app->output, quiz_id);
    }
    if (error == LBDB_OK && reveal_answer) {
        LBDB_JSON(app, lbdb_json_key(app->output, "expected_answer"));
        LBDB_JSON(app, lbdb_json_string(app->output, expected_answer));
        LBDB_JSON(app, lbdb_json_key(app->output, "grading_criteria"));
        LBDB_JSON(app, lbdb_json_raw(app->output, criteria_json));
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    free(question_state);
    free(expected_answer);
    free(criteria_json);
    lbdb_statement_destroy(statement);
    lbdb_statement_destroy(question);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_response_regrade(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *response_text = NULL;
    const char *assessment = NULL;
    const char *feedback = NULL;
    const char *reason = NULL;
    int64_t response_id = 0;
    int64_t question_id = 0;
    int64_t quiz_id = 0;
    char *old_assessment = NULL;
    char *old_feedback = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
#define REGRADE_OPTION(name, target)                                                               \
    do {                                                                                           \
        if (error == LBDB_OK) {                                                                    \
            error = lbdb_args_option(&args, (name), true, &(target));                              \
        }                                                                                          \
    } while (false)
    REGRADE_OPTION("--response", response_text);
    REGRADE_OPTION("--assessment", assessment);
    REGRADE_OPTION("--feedback", feedback);
    REGRADE_OPTION("--reason", reason);
#undef REGRADE_OPTION
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(app, "--response", response_text, &response_id);
    }
    if (error == LBDB_OK &&
        !lbdb_string_in_set(assessment, assessment_values,
                            sizeof(assessment_values) / sizeof(assessment_values[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown assessment: %s", assessment);
    }
    if (error == LBDB_OK && (!has_text(feedback) || !has_text(reason))) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION,
                              "Regrade feedback and reason must not be empty");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT r.question_id,q.quiz_id,r.assessment,r.feedback FROM quiz_responses r "
            "JOIN quiz_questions q ON q.id=r.question_id WHERE r.id=?1",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, response_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, &has_row),
                               "Cannot load quiz response");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz response: %lld",
                              (long long)response_id);
    }
    if (error == LBDB_OK) {
        question_id = lbdb_statement_column_int64(statement, 0);
        quiz_id = lbdb_statement_column_int64(statement, 1);
        old_assessment = lbdb_string_duplicate(lbdb_statement_column_text(statement, 2));
        old_feedback = lbdb_string_duplicate(lbdb_statement_column_text(statement, 3));
        if (old_assessment == NULL || old_feedback == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate old response grade");
        }
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database, "UPDATE quiz_responses SET assessment=?1,feedback=?2 WHERE id=?3",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, assessment);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, feedback);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 3, response_id);
    }
    if (error == LBDB_OK) {
        error = database_error(app, database, lbdb_statement_step(statement, NULL),
                               "Cannot update response grade");
    }
    LbdbJsonWriter *change = NULL;
    if (error == LBDB_OK) {
        change = lbdb_json_writer_create(false);
        if (change == NULL || !lbdb_json_begin_object(change) || !lbdb_json_key(change, "before") ||
            !lbdb_json_begin_object(change) || !lbdb_json_key(change, "assessment") ||
            !lbdb_json_string(change, old_assessment) || !lbdb_json_key(change, "feedback") ||
            !lbdb_json_string(change, old_feedback) || !lbdb_json_end_object(change) ||
            !lbdb_json_key(change, "after") || !lbdb_json_begin_object(change) ||
            !lbdb_json_key(change, "assessment") || !lbdb_json_string(change, assessment) ||
            !lbdb_json_key(change, "feedback") || !lbdb_json_string(change, feedback) ||
            !lbdb_json_end_object(change) || !lbdb_json_end_object(change)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build regrade history");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_add_quiz_event(app, database, quiz_id, question_id, response_id,
                                    "response_regraded", reason, lbdb_json_data(change));
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "response_id") || !lbdb_json_int(details, response_id) ||
            !lbdb_json_key(details, "reason") || !lbdb_json_string(details, reason) ||
            !lbdb_json_key(details, "change") || !lbdb_json_raw(details, lbdb_json_data(change)) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build regrade audit");
        } else {
            error = lbdb_commit_write(app, database, "response.regrade", "quiz_response",
                                      response_id, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "response.regrade");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "response_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, response_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "before"));
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "assessment"));
        LBDB_JSON(app, lbdb_json_string(app->output, old_assessment));
        LBDB_JSON(app, lbdb_json_key(app->output, "feedback"));
        LBDB_JSON(app, lbdb_json_string(app->output, old_feedback));
        LBDB_JSON(app, lbdb_json_end_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "after"));
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "assessment"));
        LBDB_JSON(app, lbdb_json_string(app->output, assessment));
        LBDB_JSON(app, lbdb_json_key(app->output, "feedback"));
        LBDB_JSON(app, lbdb_json_string(app->output, feedback));
        LBDB_JSON(app, lbdb_json_end_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "reason"));
        LBDB_JSON(app, lbdb_json_string(app->output, reason));
        LBDB_JSON(app, lbdb_json_key(app->output, "progress"));
        error = write_progress(app, database, app->output, quiz_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_json_writer_destroy(change);
    free(old_assessment);
    free(old_feedback);
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_render_quiz_progress(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                    int64_t quiz_id) {
    return write_progress(app, database, writer, quiz_id);
}

LbdbError lbdb_render_session_summary(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                      LbdbStatement *session) {
    return write_session_summary(app, database, writer, session);
}

LbdbError lbdb_render_public_question(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                      int64_t question_id) {
    return write_public_question(app, database, writer, question_id);
}
