#ifndef LEARN_BOOK_DB_JSON_H
#define LEARN_BOOK_DB_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct LbdbJsonWriter LbdbJsonWriter;

LbdbJsonWriter *lbdb_json_writer_create(bool pretty);
void lbdb_json_writer_destroy(LbdbJsonWriter *writer);
bool lbdb_json_writer_reset(LbdbJsonWriter *writer, bool pretty);
bool lbdb_json_begin_object(LbdbJsonWriter *writer);
bool lbdb_json_end_object(LbdbJsonWriter *writer);
bool lbdb_json_begin_array(LbdbJsonWriter *writer);
bool lbdb_json_end_array(LbdbJsonWriter *writer);
bool lbdb_json_key(LbdbJsonWriter *writer, const char *key);
bool lbdb_json_string(LbdbJsonWriter *writer, const char *value);
bool lbdb_json_string_or_null(LbdbJsonWriter *writer, const char *value);
bool lbdb_json_int(LbdbJsonWriter *writer, int64_t value);
bool lbdb_json_double(LbdbJsonWriter *writer, double value);
bool lbdb_json_bool(LbdbJsonWriter *writer, bool value);
bool lbdb_json_null(LbdbJsonWriter *writer);
bool lbdb_json_raw(LbdbJsonWriter *writer, const char *json);
const char *lbdb_json_data(const LbdbJsonWriter *writer);
size_t lbdb_json_size(const LbdbJsonWriter *writer);
bool lbdb_json_write(const LbdbJsonWriter *writer, FILE *stream);

#endif
