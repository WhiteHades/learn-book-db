#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool value_present(const char *value) {
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

static LbdbError statement_error(LbdbApp *app, LbdbDatabase *database, LbdbError error,
                                 const char *operation) {
    return error == LBDB_OK ? LBDB_OK : lbdb_app_database_error(app, database, error, operation);
}

static LbdbError one_id_argument(LbdbCommand *command, LbdbArgs *args, int64_t *id) {
    LbdbStringVector positionals = {0};
    LbdbError error = lbdb_args_positionals(args, &positionals);
    if (error == LBDB_OK && positionals.count != 1U) {
        error = lbdb_app_fail(command->app, LBDB_ERROR_USAGE, "%s requires exactly one ID",
                              command->key);
    }
    if (error == LBDB_OK) {
        error = lbdb_parse_positive_int(command->app, "ID", positionals.items[0], id);
    }
    lbdb_string_vector_destroy(&positionals);
    return error;
}

static LbdbError write_tag_details(LbdbApp *app, LbdbDatabase *database, LbdbJsonWriter *writer,
                                   LbdbStatement *tag) {
    const int64_t tag_id = lbdb_statement_column_int64(tag, 0);
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = LBDB_OK;
    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
        !lbdb_json_int(writer, tag_id) || !lbdb_json_key(writer, "kind") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(tag, 1)) ||
        !lbdb_json_key(writer, "name") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(tag, 2)) ||
        !lbdb_json_key(writer, "description") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(tag, 3)) ||
        !lbdb_json_key(writer, "question_count") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(tag, 4)) ||
        !lbdb_json_key(writer, "aliases") || !lbdb_json_begin_array(writer)) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag output");
    }
    error = lbdb_statement_prepare(
        database, "SELECT alias FROM tag_aliases WHERE tag_id=?1 ORDER BY alias COLLATE NOCASE",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, tag_id);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_string(writer, lbdb_statement_column_text(statement, 0))) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag aliases");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list tag aliases");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK && (!lbdb_json_end_array(writer) || !lbdb_json_key(writer, "parents") ||
                             !lbdb_json_begin_array(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag parents");
    }
    if (error == LBDB_OK) {
        error =
            lbdb_statement_prepare(database,
                                   "SELECT p.id,p.kind,p.name,r.relation_type FROM tag_relations r "
                                   "JOIN tags p ON p.id=r.parent_tag_id WHERE r.child_tag_id=?1 "
                                   "ORDER BY r.relation_type,p.kind,p.name",
                                   &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, tag_id);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
            !lbdb_json_key(writer, "kind") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
            !lbdb_json_key(writer, "name") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
            !lbdb_json_key(writer, "relation_type") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 3)) ||
            !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag parents");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list tag parents");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK && (!lbdb_json_end_array(writer) || !lbdb_json_key(writer, "children") ||
                             !lbdb_json_begin_array(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag children");
    }
    if (error == LBDB_OK) {
        error =
            lbdb_statement_prepare(database,
                                   "SELECT c.id,c.kind,c.name,r.relation_type FROM tag_relations r "
                                   "JOIN tags c ON c.id=r.child_tag_id WHERE r.parent_tag_id=?1 "
                                   "ORDER BY r.relation_type,c.kind,c.name",
                                   &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, tag_id);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
            !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
            !lbdb_json_key(writer, "kind") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
            !lbdb_json_key(writer, "name") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
            !lbdb_json_key(writer, "relation_type") ||
            !lbdb_json_string(writer, lbdb_statement_column_text(statement, 3)) ||
            !lbdb_json_end_object(writer)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build tag children");
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list tag children");
    }
    lbdb_statement_destroy(statement);
    if (error == LBDB_OK && (!lbdb_json_end_array(writer) || !lbdb_json_end_object(writer))) {
        error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize tag output");
    }
    return error;
}

LbdbError lbdb_command_tag_list(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *kind = NULL;
    const char *search = NULL;
    char *pattern = NULL;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    int64_t count = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--kind", false, &kind);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--search", false, &search);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (search != NULL) {
        pattern = lbdb_string_format("%%%s%%", search);
        if (pattern == NULL && error == LBDB_OK) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate tag search");
        }
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT t.id,t.kind,t.name,t.description,count(DISTINCT qt.question_id) "
            "FROM tags t LEFT JOIN question_tags qt ON qt.tag_id=t.id "
            "WHERE (?1 IS NULL OR t.kind=?1) AND (?2 IS NULL OR t.name LIKE ?2 OR "
            "t.description LIKE ?2) GROUP BY t.id ORDER BY t.kind,t.name",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, kind);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, pattern);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "tag.list");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "tags"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = write_tag_details(app, database, app->output, statement);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list tags");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_end_array(app->output));
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, count));
        error = lbdb_output_end(app);
    }
    free(pattern);
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError tag_alias_change(LbdbCommand *command, bool add) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *tag_reference = NULL;
    const char *alias = NULL;
    int64_t tag_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--tag", true, &tag_reference);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--alias", true, &alias);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK && !value_present(alias)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Tag alias must not be empty");
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_tag_id(app, database, tag_reference, &tag_id);
    }
    if (error == LBDB_OK && add) {
        bool has_row = false;
        error = lbdb_statement_prepare(
            database, "SELECT 1 FROM tag_aliases WHERE alias=?1 COLLATE NOCASE", &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, alias);
        }
        if (error == LBDB_OK) {
            error = statement_error(app, database, lbdb_statement_step(statement, &has_row),
                                    "Cannot inspect tag alias");
        }
        if (error == LBDB_OK && has_row) {
            error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Tag alias already exists: %s", alias);
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            add ? "INSERT INTO tag_aliases(tag_id,alias) VALUES(?1,?2)"
                : "DELETE FROM tag_aliases WHERE tag_id=?1 AND alias=?2 COLLATE NOCASE",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, tag_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 2, alias);
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(statement, NULL),
                                add ? "Cannot add tag alias" : "Cannot remove tag alias");
    }
    if (error == LBDB_OK && !add && lbdb_statement_changes(statement) != 1) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Tag alias does not exist: %s", alias);
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "tag_id") || !lbdb_json_int(details, tag_id) ||
            !lbdb_json_key(details, "alias") || !lbdb_json_string(details, alias) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build alias audit record");
        } else {
            error = lbdb_commit_write(app, database, add ? "tag.alias-add" : "tag.alias-remove",
                                      "tag", tag_id, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, add ? "tag.alias-add" : "tag.alias-remove");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "tag_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, tag_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "alias"));
        LBDB_JSON(app, lbdb_json_string(app->output, alias));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_tag_alias_add(LbdbCommand *command) {
    return tag_alias_change(command, true);
}

LbdbError lbdb_command_tag_alias_remove(LbdbCommand *command) {
    return tag_alias_change(command, false);
}

static LbdbError tag_relation_change(LbdbCommand *command, bool add) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *parent_reference = NULL;
    const char *child_reference = NULL;
    const char *type = NULL;
    int64_t parent_id = 0;
    int64_t child_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--parent", true, &parent_reference);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--child", true, &child_reference);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--type", false, &type);
    }
    if (type == NULL) {
        type = "contains";
    }
    if (error == LBDB_OK && !value_present(type)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Relation type must not be empty");
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_tag_id(app, database, parent_reference, &parent_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_resolve_tag_id(app, database, child_reference, &child_id);
    }
    if (error == LBDB_OK && parent_id == child_id) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "A tag cannot relate to itself");
    }
    if (error == LBDB_OK && add) {
        bool has_row = false;
        error = lbdb_statement_prepare(
            database,
            "SELECT 1 FROM tag_relations WHERE parent_tag_id=?1 AND child_tag_id=?2 "
            "AND relation_type=?3",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 1, parent_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 2, child_id);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 3, type);
        }
        if (error == LBDB_OK) {
            error = statement_error(app, database, lbdb_statement_step(statement, &has_row),
                                    "Cannot inspect tag relation");
        }
        if (error == LBDB_OK && has_row) {
            error = lbdb_app_fail(app, LBDB_ERROR_CONFLICT, "Tag relation already exists");
        }
        lbdb_statement_destroy(statement);
        statement = NULL;
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            add ? "INSERT INTO tag_relations(parent_tag_id,child_tag_id,relation_type,"
                  "metadata_json) VALUES(?1,?2,?3,'{}')"
                : "DELETE FROM tag_relations WHERE parent_tag_id=?1 AND child_tag_id=?2 "
                  "AND relation_type=?3",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, parent_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, child_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 3, type);
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(statement, NULL),
                                add ? "Cannot add tag relation" : "Cannot remove tag relation");
    }
    if (error == LBDB_OK && !add && lbdb_statement_changes(statement) != 1) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Tag relation does not exist");
    }
    if (error == LBDB_OK) {
        LbdbJsonWriter *details = lbdb_json_writer_create(false);
        if (details == NULL || !lbdb_json_begin_object(details) ||
            !lbdb_json_key(details, "parent_id") || !lbdb_json_int(details, parent_id) ||
            !lbdb_json_key(details, "child_id") || !lbdb_json_int(details, child_id) ||
            !lbdb_json_key(details, "type") || !lbdb_json_string(details, type) ||
            !lbdb_json_end_object(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build relation audit");
        } else {
            error =
                lbdb_commit_write(app, database, add ? "tag.relation-add" : "tag.relation-remove",
                                  "tag_relation", 0, lbdb_json_data(details));
        }
        lbdb_json_writer_destroy(details);
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, add ? "tag.relation-add" : "tag.relation-remove");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "parent_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, parent_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "child_id"));
        LBDB_JSON(app, lbdb_json_int(app->output, child_id));
        LBDB_JSON(app, lbdb_json_key(app->output, "relation_type"));
        LBDB_JSON(app, lbdb_json_string(app->output, type));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

LbdbError lbdb_command_tag_relation_add(LbdbCommand *command) {
    return tag_relation_change(command, true);
}

LbdbError lbdb_command_tag_relation_remove(LbdbCommand *command) {
    return tag_relation_change(command, false);
}

static LbdbError get_or_create_template(LbdbApp *app, LbdbDatabase *database,
                                        const char *scope_type, const char *title, int64_t unit_id,
                                        int64_t tag_id, double checkpoint,
                                        const char *selection_policy, int64_t *template_id) {
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database,
        "SELECT id FROM quiz_templates WHERE scope_type=?1 AND ifnull(unit_id,0)=?2 "
        "AND ifnull(tag_id,0)=?3 AND ifnull(checkpoint,0)=?4",
        &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, scope_type);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, unit_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 3, tag_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_double(statement, 4, checkpoint);
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(statement, &has_row),
                                "Cannot find quiz template");
    }
    if (error == LBDB_OK && has_row) {
        *template_id = lbdb_statement_column_int64(statement, 0);
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK && has_row) {
        error = lbdb_statement_prepare(
            database, "UPDATE quiz_templates SET title=?1,selection_policy=?2,active=1 WHERE id=?3",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, title);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, selection_policy);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(statement, 3, *template_id);
        }
    } else if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_templates(scope_type,title,unit_id,tag_id,checkpoint,"
            "selection_policy,active,metadata_json) VALUES(?1,?2,?3,?4,?5,?6,1,'{}')",
            &statement);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 1, scope_type);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 2, title);
        }
        if (error == LBDB_OK) {
            error = unit_id > 0 ? lbdb_statement_bind_int64(statement, 3, unit_id)
                                : lbdb_statement_bind_null(statement, 3);
        }
        if (error == LBDB_OK) {
            error = tag_id > 0 ? lbdb_statement_bind_int64(statement, 4, tag_id)
                               : lbdb_statement_bind_null(statement, 4);
        }
        if (error == LBDB_OK) {
            error = checkpoint > 0.0 ? lbdb_statement_bind_double(statement, 5, checkpoint)
                                     : lbdb_statement_bind_null(statement, 5);
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_text(statement, 6, selection_policy);
        }
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(statement, NULL),
                                "Cannot store quiz template");
    }
    if (error == LBDB_OK && !has_row) {
        *template_id = lbdb_statement_last_insert_id(statement);
    }
    lbdb_statement_destroy(statement);
    return error;
}

static LbdbError replace_template_questions(LbdbApp *app, LbdbDatabase *database,
                                            int64_t template_id, int64_t filter_id,
                                            double checkpoint, bool by_tag,
                                            int64_t *question_count) {
    LbdbStatement *statement = NULL;
    LbdbStatement *insert = NULL;
    bool has_row = false;
    LbdbError error = lbdb_statement_prepare(
        database, "DELETE FROM quiz_template_questions WHERE template_id=?1", &statement);
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, template_id);
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(statement, NULL),
                                "Cannot clear template questions");
    }
    lbdb_statement_destroy(statement);
    statement = NULL;
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            by_tag ? "SELECT q.id FROM question_bank q JOIN question_tags qt ON "
                     "qt.question_id=q.id WHERE qt.tag_id=?1 AND q.active=1 "
                     "ORDER BY q.unit_id,q.position"
                   : "SELECT id FROM question_bank WHERE unit_id=?1 AND active=1 "
                     "AND earliest_checkpoint<=?2 ORDER BY position",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 1, filter_id);
    }
    if (error == LBDB_OK && !by_tag) {
        error = lbdb_statement_bind_double(statement, 2, checkpoint);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "INSERT INTO quiz_template_questions(template_id,question_id,position) "
            "VALUES(?1,?2,?3)",
            &insert);
    }
    int64_t position = 0;
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        position += 1;
        error = lbdb_statement_bind_int64(insert, 1, template_id);
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 2, lbdb_statement_column_int64(statement, 0));
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_bind_int64(insert, 3, position);
        }
        if (error == LBDB_OK) {
            error = statement_error(app, database, lbdb_statement_step(insert, NULL),
                                    "Cannot link template question");
        }
        if (error == LBDB_OK) {
            error = lbdb_statement_reset(insert);
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error =
            lbdb_app_database_error(app, database, error, "Cannot enumerate template questions");
    }
    *question_count = position;
    lbdb_statement_destroy(statement);
    lbdb_statement_destroy(insert);
    return error;
}

LbdbError lbdb_command_template_rebuild(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    LbdbDatabase *database = NULL;
    LbdbStatement *units = NULL;
    LbdbStatement *tags = NULL;
    bool has_row = false;
    int64_t template_count = 0;
    int64_t link_count = 0;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_begin_write(app, &database);
    }
    if (error == LBDB_OK) {
        error = statement_error(
            app, database,
            lbdb_database_exec_static(database, "UPDATE quiz_templates SET active=0"),
            "Cannot deactivate old templates");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT id,title FROM source_units u WHERE include_in_quizzes=1 AND EXISTS("
            "SELECT 1 FROM question_bank q WHERE q.unit_id=u.id AND q.active=1) "
            "ORDER BY corpus_slug,position",
            &units);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(units, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const int64_t unit_id = lbdb_statement_column_int64(units, 0);
        const char *unit_title = lbdb_statement_column_text(units, 1);
        static const double checkpoints[] = {0.25, 0.5, 0.75, 1.0};
        for (size_t index = 0; error == LBDB_OK && index < 4U; ++index) {
            const bool final = checkpoints[index] == 1.0;
            char *title = final ? lbdb_string_format("%s - Final", unit_title)
                                : lbdb_string_format("%s - %d%% checkpoint", unit_title,
                                                     (int)(checkpoints[index] * 100.0));
            int64_t template_id = 0;
            int64_t questions = 0;
            if (title == NULL) {
                error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate template title");
            }
            if (error == LBDB_OK) {
                error = get_or_create_template(app, database, final ? "final" : "checkpoint", title,
                                               unit_id, 0, checkpoints[index],
                                               "cumulative_balanced_weighted", &template_id);
            }
            if (error == LBDB_OK) {
                error = replace_template_questions(app, database, template_id, unit_id,
                                                   checkpoints[index], false, &questions);
            }
            if (error == LBDB_OK) {
                template_count += 1;
                link_count += questions;
            }
            free(title);
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot enumerate source units");
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT t.id,t.kind,t.name FROM tags t WHERE lower(t.kind) IN('topic','theme') "
            "AND EXISTS(SELECT 1 FROM question_tags qt JOIN question_bank q ON "
            "q.id=qt.question_id WHERE qt.tag_id=t.id AND q.active=1) ORDER BY t.kind,t.name",
            &tags);
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(tags, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        const int64_t tag_id = lbdb_statement_column_int64(tags, 0);
        const char *kind = lbdb_statement_column_text(tags, 1);
        char *title = lbdb_string_format("%s quick quiz", lbdb_statement_column_text(tags, 2));
        int64_t template_id = 0;
        int64_t questions = 0;
        if (title == NULL) {
            error = lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate template title");
        }
        if (error == LBDB_OK) {
            error = get_or_create_template(app, database, kind, title, 0, tag_id, 0.0,
                                           "adaptive_prioritize_unseen_and_weak", &template_id);
        }
        if (error == LBDB_OK) {
            error = replace_template_questions(app, database, template_id, tag_id, 0.0, true,
                                               &questions);
        }
        if (error == LBDB_OK) {
            template_count += 1;
            link_count += questions;
        }
        free(title);
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot enumerate template tags");
    }
    if (error == LBDB_OK) {
        char details[128] = {0};
        const int length =
            snprintf(details, sizeof(details), "{\"templates\":%lld,\"question_links\":%lld}",
                     (long long)template_count, (long long)link_count);
        if (length <= 0 || (size_t)length >= sizeof(details)) {
            error = lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Cannot format template audit");
        } else {
            error =
                lbdb_commit_write(app, database, "template.rebuild", "quiz_template", 0, details);
        }
    }
    if (error != LBDB_OK && database != NULL) {
        lbdb_database_rollback(database);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "template.rebuild");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "count"));
        LBDB_JSON(app, lbdb_json_int(app->output, template_count));
        LBDB_JSON(app, lbdb_json_key(app->output, "question_links"));
        LBDB_JSON(app, lbdb_json_int(app->output, link_count));
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(units);
    lbdb_statement_destroy(tags);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}

static LbdbError write_template_summary(LbdbApp *app, LbdbJsonWriter *writer,
                                        LbdbStatement *statement) {
    if (!lbdb_json_begin_object(writer) || !lbdb_json_key(writer, "id") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 0)) ||
        !lbdb_json_key(writer, "scope_type") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 1)) ||
        !lbdb_json_key(writer, "title") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 2)) ||
        !lbdb_json_key(writer, "unit_id") ||
        (lbdb_statement_column_is_null(statement, 3)
             ? !lbdb_json_null(writer)
             : !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 3))) ||
        !lbdb_json_key(writer, "tag_id") ||
        (lbdb_statement_column_is_null(statement, 4)
             ? !lbdb_json_null(writer)
             : !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 4))) ||
        !lbdb_json_key(writer, "checkpoint")) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build template output");
    }
    if (lbdb_statement_column_is_null(statement, 5)) {
        if (!lbdb_json_null(writer)) {
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build template output");
        }
    } else if (!lbdb_json_double(writer, lbdb_statement_column_double(statement, 5))) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to build template output");
    }
    if (!lbdb_json_key(writer, "selection_policy") ||
        !lbdb_json_string(writer, lbdb_statement_column_text(statement, 6)) ||
        !lbdb_json_key(writer, "active") ||
        !lbdb_json_bool(writer, lbdb_statement_column_int64(statement, 7) != 0) ||
        !lbdb_json_key(writer, "question_count") ||
        !lbdb_json_int(writer, lbdb_statement_column_int64(statement, 8)) ||
        !lbdb_json_end_object(writer)) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to finalize template output");
    }
    return LBDB_OK;
}

LbdbError lbdb_command_template_list(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    const char *scope = NULL;
    const char *unit_ref = NULL;
    const char *tag_ref = NULL;
    const char *active = NULL;
    int64_t unit_id = 0;
    int64_t tag_id = 0;
    int64_t count = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *statement = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--scope", false, &scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--unit", false, &unit_ref);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--tag", false, &tag_ref);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_option(&args, "--active", false, &active);
    }
    if (active == NULL) {
        active = "active";
    }
    static const char *const scopes[] = {"checkpoint", "final", "theme", "topic"};
    static const char *const active_values[] = {"active", "all", "inactive"};
    if (error == LBDB_OK && scope != NULL &&
        !lbdb_string_in_set(scope, scopes, sizeof(scopes) / sizeof(scopes[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "Unknown template scope: %s", scope);
    }
    if (error == LBDB_OK && !lbdb_string_in_set(active, active_values,
                                                sizeof(active_values) / sizeof(active_values[0]))) {
        error = lbdb_app_fail(app, LBDB_ERROR_USAGE, "--active must be active, inactive, or all");
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK && unit_ref != NULL) {
        error = lbdb_resolve_unit_id(app, database, unit_ref, &unit_id);
    }
    if (error == LBDB_OK && tag_ref != NULL) {
        error = lbdb_resolve_tag_id(app, database, tag_ref, &tag_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT t.id,t.scope_type,t.title,t.unit_id,t.tag_id,t.checkpoint,"
            "t.selection_policy,t.active,(SELECT count(*) FROM quiz_template_questions q "
            "WHERE q.template_id=t.id) FROM quiz_templates t WHERE "
            "(?1 IS NULL OR t.scope_type=?1) AND (?2=0 OR t.unit_id=?2) AND "
            "(?3=0 OR t.tag_id=?3) AND (?4='all' OR (?4='active' AND t.active=1) OR "
            "(?4='inactive' AND t.active=0)) ORDER BY t.scope_type,t.title,t.id",
            &statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 1, scope);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 2, unit_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(statement, 3, tag_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_text(statement, 4, active);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "template.list");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "templates"));
        LBDB_JSON(app, lbdb_json_begin_array(app->output));
    }
    while (error == LBDB_OK) {
        error = lbdb_statement_step(statement, &has_row);
        if (error != LBDB_OK || !has_row) {
            break;
        }
        error = write_template_summary(app, app->output, statement);
        if (error == LBDB_OK) {
            count += 1;
        }
    }
    if (error == LBDB_ERROR_SQLITE) {
        error = lbdb_app_database_error(app, database, error, "Cannot list templates");
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

LbdbError lbdb_command_template_show(LbdbCommand *command) {
    LbdbApp *app = command->app;
    LbdbArgs args = {0};
    int64_t template_id = 0;
    LbdbDatabase *database = NULL;
    LbdbStatement *template_statement = NULL;
    LbdbStatement *questions = NULL;
    bool has_row = false;
    LbdbError error = lbdb_args_init(&args, command);
    if (error == LBDB_OK) {
        error = one_id_argument(command, &args, &template_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_args_finish(&args);
    }
    if (error == LBDB_OK) {
        error = lbdb_app_open_database(app, LBDB_DATABASE_READ_ONLY, &database);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT t.id,t.scope_type,t.title,t.unit_id,t.tag_id,t.checkpoint,"
            "t.selection_policy,t.active,(SELECT count(*) FROM quiz_template_questions q "
            "WHERE q.template_id=t.id) FROM quiz_templates t WHERE t.id=?1",
            &template_statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(template_statement, 1, template_id);
    }
    if (error == LBDB_OK) {
        error = statement_error(app, database, lbdb_statement_step(template_statement, &has_row),
                                "Cannot load template");
    }
    if (error == LBDB_OK && !has_row) {
        error = lbdb_app_fail(app, LBDB_ERROR_NOT_FOUND, "Unknown quiz template: %lld",
                              (long long)template_id);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_begin(app, "template.show");
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "template"));
        error = write_template_summary(app, app->output, template_statement);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_prepare(
            database,
            "SELECT l.position,q.id,q.question_type,q.response_format,q.prompt,q.active "
            "FROM quiz_template_questions l JOIN question_bank q ON q.id=l.question_id "
            "WHERE l.template_id=?1 ORDER BY l.position",
            &questions);
    }
    if (error == LBDB_OK) {
        error = lbdb_statement_bind_int64(questions, 1, template_id);
    }
    if (error == LBDB_OK) {
        LBDB_JSON(app, lbdb_json_key(app->output, "questions"));
        error = lbdb_output_statement_rows(app, questions);
    }
    if (error == LBDB_OK) {
        error = lbdb_output_end(app);
    }
    lbdb_statement_destroy(questions);
    lbdb_statement_destroy(template_statement);
    lbdb_database_close(database);
    lbdb_args_destroy(&args);
    return error;
}
