#include "internal.h"

#include <stdlib.h>
#include <string.h>

static const LbdbCommandEntry command_entries[] = {
    {"bank.activate", lbdb_command_bank_activate, "Reactivate a retired bank question",
     "bank activate ID"},
    {"bank.export", lbdb_command_bank_export, "Export one unit bank",
     "bank export --unit REF --output PATH"},
    {"bank.export-all", lbdb_command_bank_export_all, "Export every populated unit bank",
     "bank export-all --output-dir DIR"},
    {"bank.import", lbdb_command_bank_import, "Import one or more source-grounded banks",
     "bank import FILE..."},
    {"bank.retire", lbdb_command_bank_retire, "Retire a canonical bank question",
     "bank retire ID --reason TEXT"},
    {"bank.revise", lbdb_command_bank_revise, "Revise a canonical bank question",
     "bank revise ID --input JSON_OR_@PATH"},
    {"bank.search", lbdb_command_bank_search, "Search bank questions through fixed filters",
     "bank search [--unit REF] [--tag REF]... [--tag-kind KIND] [--question-type TYPE] "
     "[--response-format FORMAT] [--active active|retired|all] [--text TEXT] [--limit N]"},
    {"bank.show", lbdb_command_bank_show, "Show one bank question and rubric", "bank show ID"},
    {"bank.validate", lbdb_command_bank_validate, "Validate source and bank invariants",
     "bank validate [--allow-incomplete]"},
    {"corpus.status", lbdb_command_corpus_status, "Compare manifest, files, and stored hashes",
     "corpus status"},
    {"corpus.sync", lbdb_command_corpus_sync, "Synchronize manifest units and sections",
     "corpus sync"},
    {"db.backup", lbdb_command_db_backup, "Create a consistent SQLite backup", "db backup PATH"},
    {"db.doctor", lbdb_command_db_doctor, "Run schema, JSON, integrity, and FK checks",
     "db doctor"},
    {"db.init", lbdb_command_db_init, "Create a fresh schema-v1 database", "db init"},
    {"db.restore", lbdb_command_db_restore, "Validate and restore a database backup",
     "db restore PATH --yes"},
    {"db.status", lbdb_command_db_status, "Show identity, versions, and row counts", "db status"},
    {"learning.add", lbdb_command_learning_add, "Append chronological learning evidence",
     "learning add --topic TEXT --status STATE --evidence TEXT [--next-step TEXT] "
     "[--source-type TYPE] [--quiz ID] [--response ID]"},
    {"learning.list", lbdb_command_learning_list, "List learning evidence",
     "learning list [--status STATE] [--topic TEXT] [--quiz ID] [--limit N]"},
    {"learning.show", lbdb_command_learning_show, "Show one learning record", "learning show ID"},
    {"quiz.abandon", lbdb_command_quiz_abandon, "Abandon an active quiz session",
     "quiz abandon ID --reason TEXT"},
    {"quiz.complete", lbdb_command_quiz_complete, "Complete a fully resolved session",
     "quiz complete ID"},
    {"quiz.defer", lbdb_command_quiz_defer, "Defer the current question",
     "quiz defer ID --question ID --reason TEXT"},
    {"quiz.follow-up", lbdb_command_quiz_follow_up, "Append a source-grounded follow-up",
     "quiz follow-up ID --input JSON_OR_@PATH"},
    {"quiz.list", lbdb_command_quiz_list, "List quiz sessions",
     "quiz list [--status STATE] [--scope SCOPE]"},
    {"quiz.next", lbdb_command_quiz_next, "Atomically ask or return the current question",
     "quiz next ID"},
    {"quiz.pause", lbdb_command_quiz_pause, "Pause an in-progress session",
     "quiz pause ID [--reason TEXT]"},
    {"quiz.requeue", lbdb_command_quiz_requeue, "Return an asked or deferred question to planned",
     "quiz requeue ID --question ID"},
    {"quiz.resume", lbdb_command_quiz_resume, "Resume a paused session", "quiz resume ID"},
    {"quiz.start", lbdb_command_quiz_start, "Start or return an active template session",
     "quiz start --template ID [--limit N]"},
    {"quiz.status", lbdb_command_quiz_status, "Show one session and current question",
     "quiz status ID"},
    {"report.active", lbdb_command_report_active, "Report active quiz sessions", "report active"},
    {"report.coverage", lbdb_command_report_coverage, "Report source and section coverage",
     "report coverage"},
    {"report.mastery", lbdb_command_report_mastery, "Report latest learning and assessment states",
     "report mastery"},
    {"report.quiz", lbdb_command_report_quiz, "Report one quiz snapshot and history",
     "report quiz ID"},
    {"report.state-drift", lbdb_command_report_state_drift, "Detect lifecycle state drift",
     "report state-drift"},
    {"response.regrade", lbdb_command_response_regrade, "Regrade a response with audit history",
     "response regrade --response ID --assessment ASSESSMENT --feedback TEXT --reason TEXT"},
    {"response.submit", lbdb_command_response_submit, "Store an assessed response",
     "response submit --question ID --answer TEXT --assessment ASSESSMENT --feedback TEXT "
     "[--learning-topic TEXT --learning-status STATE --evidence TEXT] [--next-step TEXT] "
     "[--reveal-answer]"},
    {"stats", lbdb_command_stats, "Show aggregate database counts", "stats"},
    {"tag.alias-add", lbdb_command_tag_alias_add, "Add a globally unique tag alias",
     "tag alias-add --tag REF --alias ALIAS"},
    {"tag.alias-remove", lbdb_command_tag_alias_remove, "Remove a tag alias",
     "tag alias-remove --tag REF --alias ALIAS"},
    {"tag.list", lbdb_command_tag_list, "List tags, aliases, and relations",
     "tag list [--kind KIND] [--search TEXT]"},
    {"tag.relation-add", lbdb_command_tag_relation_add, "Add a directed tag relation",
     "tag relation-add --parent REF --child REF [--type TYPE]"},
    {"tag.relation-remove", lbdb_command_tag_relation_remove, "Remove a directed tag relation",
     "tag relation-remove --parent REF --child REF [--type TYPE]"},
    {"template.list", lbdb_command_template_list, "List quiz templates",
     "template list [--scope SCOPE] [--unit REF] [--tag REF] "
     "[--active active|inactive|all]"},
    {"template.rebuild", lbdb_command_template_rebuild, "Rebuild materialized templates",
     "template rebuild"},
    {"template.show", lbdb_command_template_show, "Show a template and question order",
     "template show ID"},
};

static const LbdbCommandEntry *find_command(const char *key) {
    size_t lower = 0U;
    size_t upper = sizeof(command_entries) / sizeof(command_entries[0]);
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2U;
        const int comparison = strcmp(key, command_entries[middle].key);
        if (comparison == 0) {
            return &command_entries[middle];
        }
        if (comparison < 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return NULL;
}

LbdbError lbdb_command_create(LbdbApp *app, int argc, char *const argv[],
                              LbdbCommand **out_command) {
    LbdbCommand *command = NULL;
    int consumed = 0;
    char *key = NULL;
    const LbdbCommandEntry *entry = NULL;

    if (out_command == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Invalid command output");
    }
    *out_command = NULL;
    if (argc < 1) {
        return lbdb_app_fail(app, LBDB_ERROR_USAGE, "A command group is required; use --help");
    }
    if (strcmp(argv[0], "stats") == 0) {
        key = lbdb_string_duplicate("stats");
        consumed = 1;
    } else {
        if (argc < 2) {
            return lbdb_app_fail(app, LBDB_ERROR_USAGE, "Command group %s needs an action",
                                 argv[0]);
        }
        key = lbdb_string_format("%s.%s", argv[0], argv[1]);
        consumed = 2;
    }
    if (key == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate command name");
    }
    entry = find_command(key);
    if (entry == NULL) {
        LbdbError error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown command: %s", key);
        free(key);
        return error;
    }
    command = calloc(1U, sizeof(*command));
    if (command == NULL) {
        free(key);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate command");
    }
    command->app = app;
    command->entry = entry;
    command->key = key;
    command->argc = argc - consumed;
    command->argv = calloc((size_t)command->argc + 1U, sizeof(*command->argv));
    if (command->argv == NULL) {
        lbdb_command_destroy(command);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate command arguments");
    }
    for (int index = 0; index < command->argc; ++index) {
        command->argv[index] = lbdb_string_duplicate(argv[index + consumed]);
        if (command->argv[index] == NULL) {
            lbdb_command_destroy(command);
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to copy command arguments");
        }
    }
    *out_command = command;
    return LBDB_OK;
}

void lbdb_command_destroy(LbdbCommand *command) {
    if (command == NULL) {
        return;
    }
    if (command->argv != NULL) {
        for (int index = 0; index < command->argc; ++index) {
            free(command->argv[index]);
        }
    }
    free(command->argv);
    free(command->key);
    free(command);
}

LbdbError lbdb_command_execute(LbdbCommand *command) {
    return command == NULL || command->entry == NULL ? LBDB_ERROR_INTERNAL
                                                     : command->entry->handler(command);
}

LbdbError lbdb_command_write_help(LbdbApp *app, int argc, char *const argv[]) {
    const LbdbCommandEntry *target = NULL;
    char *key = NULL;
    if (argc != 0) {
        if (argc == 1 && strcmp(argv[0], "stats") == 0) {
            key = lbdb_string_duplicate("stats");
        } else if (argc == 2) {
            key = lbdb_string_format("%s.%s", argv[0], argv[1]);
        } else {
            return lbdb_app_fail(app, LBDB_ERROR_USAGE,
                                 "Targeted help requires GROUP COMMAND or stats");
        }
        if (key == NULL) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate help target");
        }
        target = find_command(key);
        if (target == NULL) {
            LbdbError error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown command: %s", key);
            free(key);
            return error;
        }
    }
    LbdbError error = lbdb_output_begin(app, "help");
    if (error != LBDB_OK) {
        free(key);
        return error;
    }
    if (target != NULL) {
        LBDB_JSON(app, lbdb_json_key(app->output, "name"));
        LBDB_JSON(app, lbdb_json_string(app->output, target->key));
        LBDB_JSON(app, lbdb_json_key(app->output, "summary"));
        LBDB_JSON(app, lbdb_json_string(app->output, target->summary));
        LBDB_JSON(app, lbdb_json_key(app->output, "usage"));
        LBDB_JSON(app, lbdb_json_string(app->output, target->usage));
        free(key);
        return lbdb_output_end(app);
    }
    LBDB_JSON(app, lbdb_json_key(app->output, "usage"));
    LBDB_JSON(app, lbdb_json_string(app->output,
                                    "learn-book-db [--root PATH] [--db PATH] [--manifest PATH] "
                                    "[--pretty] GROUP COMMAND [OPTIONS]"));
    LBDB_JSON(app, lbdb_json_key(app->output, "defaults"));
    LBDB_JSON(app, lbdb_json_begin_object(app->output));
    LBDB_JSON(app, lbdb_json_key(app->output, "root"));
    LBDB_JSON(app, lbdb_json_string(app->output, "current directory"));
    LBDB_JSON(app, lbdb_json_key(app->output, "database"));
    LBDB_JSON(app, lbdb_json_string(app->output, LBDB_DEFAULT_DB));
    LBDB_JSON(app, lbdb_json_key(app->output, "manifest"));
    LBDB_JSON(app, lbdb_json_string(app->output, LBDB_DEFAULT_MANIFEST));
    LBDB_JSON(app, lbdb_json_end_object(app->output));
    LBDB_JSON(app, lbdb_json_key(app->output, "commands"));
    LBDB_JSON(app, lbdb_json_begin_array(app->output));
    for (size_t index = 0; index < sizeof(command_entries) / sizeof(command_entries[0]); ++index) {
        LBDB_JSON(app, lbdb_json_begin_object(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "name"));
        LBDB_JSON(app, lbdb_json_string(app->output, command_entries[index].key));
        LBDB_JSON(app, lbdb_json_key(app->output, "summary"));
        LBDB_JSON(app, lbdb_json_string(app->output, command_entries[index].summary));
        LBDB_JSON(app, lbdb_json_key(app->output, "usage"));
        LBDB_JSON(app, lbdb_json_string(app->output, command_entries[index].usage));
        LBDB_JSON(app, lbdb_json_end_object(app->output));
    }
    LBDB_JSON(app, lbdb_json_end_array(app->output));
    return lbdb_output_end(app);
}
