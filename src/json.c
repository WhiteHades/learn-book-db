#include "learn_book_db/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum LbdbJsonContextKind {
    LBDB_JSON_CONTEXT_OBJECT,
    LBDB_JSON_CONTEXT_ARRAY
} LbdbJsonContextKind;

typedef struct LbdbJsonContext {
    LbdbJsonContextKind kind;
    bool first;
    bool expecting_value;
} LbdbJsonContext;

struct LbdbJsonWriter {
    char *buffer;
    size_t size;
    size_t capacity;
    LbdbJsonContext *stack;
    size_t depth;
    size_t stack_capacity;
    bool pretty;
    bool root_written;
    bool failed;
};

static bool reserve_bytes(LbdbJsonWriter *writer, size_t additional) {
    size_t required = 0;
    size_t capacity = 0;
    char *replacement = NULL;

    if (writer->failed) {
        return false;
    }
    if (additional > SIZE_MAX - writer->size - 1U) {
        writer->failed = true;
        return false;
    }
    required = writer->size + additional + 1U;
    if (required <= writer->capacity) {
        return true;
    }
    capacity = writer->capacity == 0U ? 256U : writer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    replacement = realloc(writer->buffer, capacity);
    if (replacement == NULL) {
        writer->failed = true;
        return false;
    }
    writer->buffer = replacement;
    writer->capacity = capacity;
    return true;
}

static bool append_bytes(LbdbJsonWriter *writer, const char *value, size_t size) {
    if (!reserve_bytes(writer, size)) {
        return false;
    }
    if (size > 0U) {
        memcpy(writer->buffer + writer->size, value, size);
        writer->size += size;
    }
    writer->buffer[writer->size] = '\0';
    return true;
}

static bool append_text(LbdbJsonWriter *writer, const char *value) {
    return append_bytes(writer, value, strlen(value));
}

static bool append_indent(LbdbJsonWriter *writer, size_t depth) {
    if (!writer->pretty) {
        return true;
    }
    if (!append_text(writer, "\n")) {
        return false;
    }
    for (size_t index = 0; index < depth; ++index) {
        if (!append_text(writer, "  ")) {
            return false;
        }
    }
    return true;
}

static bool push_context(LbdbJsonWriter *writer, LbdbJsonContextKind kind) {
    LbdbJsonContext *replacement = NULL;
    size_t capacity = 0;

    if (writer->depth == writer->stack_capacity) {
        capacity = writer->stack_capacity == 0U ? 8U : writer->stack_capacity * 2U;
        if (capacity < writer->stack_capacity) {
            writer->failed = true;
            return false;
        }
        replacement = realloc(writer->stack, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            writer->failed = true;
            return false;
        }
        writer->stack = replacement;
        writer->stack_capacity = capacity;
    }
    writer->stack[writer->depth] =
        (LbdbJsonContext){.kind = kind, .first = true, .expecting_value = false};
    writer->depth += 1U;
    return true;
}

static bool before_value(LbdbJsonWriter *writer) {
    LbdbJsonContext *context = NULL;

    if (writer->depth == 0U) {
        if (writer->root_written) {
            writer->failed = true;
            return false;
        }
        writer->root_written = true;
        return true;
    }
    context = &writer->stack[writer->depth - 1U];
    if (context->kind == LBDB_JSON_CONTEXT_OBJECT) {
        if (!context->expecting_value) {
            writer->failed = true;
            return false;
        }
        context->expecting_value = false;
        return true;
    }
    if (!context->first && !append_text(writer, ",")) {
        return false;
    }
    if (!append_indent(writer, writer->depth)) {
        return false;
    }
    context->first = false;
    return true;
}

static bool append_escaped_string(LbdbJsonWriter *writer, const char *value) {
    static const char hex[] = "0123456789abcdef";

    if (!append_text(writer, "\"")) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != 0U; ++cursor) {
        const unsigned char character = *cursor;
        switch (character) {
        case '"':
            if (!append_text(writer, "\\\"")) {
                return false;
            }
            break;
        case '\\':
            if (!append_text(writer, "\\\\")) {
                return false;
            }
            break;
        case '\b':
            if (!append_text(writer, "\\b")) {
                return false;
            }
            break;
        case '\f':
            if (!append_text(writer, "\\f")) {
                return false;
            }
            break;
        case '\n':
            if (!append_text(writer, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!append_text(writer, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!append_text(writer, "\\t")) {
                return false;
            }
            break;
        default:
            if (character < 0x20U) {
                char escape[6] = {
                    '\\', 'u', '0', '0', hex[character >> 4U], hex[character & 0x0fU]};
                if (!append_bytes(writer, escape, sizeof(escape))) {
                    return false;
                }
            } else if (!append_bytes(writer, (const char *)&character, 1U)) {
                return false;
            }
            break;
        }
    }
    return append_text(writer, "\"");
}

LbdbJsonWriter *lbdb_json_writer_create(bool pretty) {
    LbdbJsonWriter *writer = calloc(1U, sizeof(*writer));
    if (writer == NULL) {
        return NULL;
    }
    writer->pretty = pretty;
    if (!reserve_bytes(writer, 0U)) {
        lbdb_json_writer_destroy(writer);
        return NULL;
    }
    return writer;
}

void lbdb_json_writer_destroy(LbdbJsonWriter *writer) {
    if (writer == NULL) {
        return;
    }
    free(writer->buffer);
    free(writer->stack);
    free(writer);
}

bool lbdb_json_writer_reset(LbdbJsonWriter *writer, bool pretty) {
    if (writer == NULL) {
        return false;
    }
    writer->size = 0U;
    writer->depth = 0U;
    writer->pretty = pretty;
    writer->root_written = false;
    writer->failed = false;
    if (!reserve_bytes(writer, 0U)) {
        return false;
    }
    writer->buffer[0] = '\0';
    return true;
}

bool lbdb_json_begin_object(LbdbJsonWriter *writer) {
    return writer != NULL && before_value(writer) && append_text(writer, "{") &&
           push_context(writer, LBDB_JSON_CONTEXT_OBJECT);
}

bool lbdb_json_end_object(LbdbJsonWriter *writer) {
    LbdbJsonContext context = {0};
    if (writer == NULL || writer->depth == 0U) {
        return false;
    }
    context = writer->stack[writer->depth - 1U];
    if (context.kind != LBDB_JSON_CONTEXT_OBJECT || context.expecting_value) {
        writer->failed = true;
        return false;
    }
    writer->depth -= 1U;
    if (!context.first && !append_indent(writer, writer->depth)) {
        return false;
    }
    return append_text(writer, "}");
}

bool lbdb_json_begin_array(LbdbJsonWriter *writer) {
    return writer != NULL && before_value(writer) && append_text(writer, "[") &&
           push_context(writer, LBDB_JSON_CONTEXT_ARRAY);
}

bool lbdb_json_end_array(LbdbJsonWriter *writer) {
    LbdbJsonContext context = {0};
    if (writer == NULL || writer->depth == 0U) {
        return false;
    }
    context = writer->stack[writer->depth - 1U];
    if (context.kind != LBDB_JSON_CONTEXT_ARRAY) {
        writer->failed = true;
        return false;
    }
    writer->depth -= 1U;
    if (!context.first && !append_indent(writer, writer->depth)) {
        return false;
    }
    return append_text(writer, "]");
}

bool lbdb_json_key(LbdbJsonWriter *writer, const char *key) {
    LbdbJsonContext *context = NULL;
    if (writer == NULL || key == NULL || writer->depth == 0U) {
        return false;
    }
    context = &writer->stack[writer->depth - 1U];
    if (context->kind != LBDB_JSON_CONTEXT_OBJECT || context->expecting_value) {
        writer->failed = true;
        return false;
    }
    if (!context->first && !append_text(writer, ",")) {
        return false;
    }
    if (!append_indent(writer, writer->depth) || !append_escaped_string(writer, key) ||
        !append_text(writer, writer->pretty ? ": " : ":")) {
        return false;
    }
    context->first = false;
    context->expecting_value = true;
    return true;
}

bool lbdb_json_string(LbdbJsonWriter *writer, const char *value) {
    return writer != NULL && value != NULL && before_value(writer) &&
           append_escaped_string(writer, value);
}

bool lbdb_json_string_or_null(LbdbJsonWriter *writer, const char *value) {
    return value == NULL ? lbdb_json_null(writer) : lbdb_json_string(writer, value);
}

bool lbdb_json_int(LbdbJsonWriter *writer, int64_t value) {
    char text[32] = {0};
    int length = 0;
    if (writer == NULL || !before_value(writer)) {
        return false;
    }
    length = snprintf(text, sizeof(text), "%lld", (long long)value);
    return length > 0 && (size_t)length < sizeof(text) &&
           append_bytes(writer, text, (size_t)length);
}

bool lbdb_json_double(LbdbJsonWriter *writer, double value) {
    char text[64] = {0};
    int length = 0;
    if (writer == NULL || !isfinite(value) || !before_value(writer)) {
        return false;
    }
    length = snprintf(text, sizeof(text), "%.17g", value);
    return length > 0 && (size_t)length < sizeof(text) &&
           append_bytes(writer, text, (size_t)length);
}

bool lbdb_json_bool(LbdbJsonWriter *writer, bool value) {
    return writer != NULL && before_value(writer) && append_text(writer, value ? "true" : "false");
}

bool lbdb_json_null(LbdbJsonWriter *writer) {
    return writer != NULL && before_value(writer) && append_text(writer, "null");
}

bool lbdb_json_raw(LbdbJsonWriter *writer, const char *json) {
    return writer != NULL && json != NULL && json[0] != '\0' && before_value(writer) &&
           append_text(writer, json);
}

const char *lbdb_json_data(const LbdbJsonWriter *writer) {
    return writer == NULL || writer->buffer == NULL ? "" : writer->buffer;
}

size_t lbdb_json_size(const LbdbJsonWriter *writer) { return writer == NULL ? 0U : writer->size; }

bool lbdb_json_write(const LbdbJsonWriter *writer, FILE *stream) {
    if (writer == NULL || stream == NULL || writer->failed || writer->depth != 0U ||
        !writer->root_written) {
        return false;
    }
    return fwrite(writer->buffer, 1U, writer->size, stream) == writer->size &&
           fputc('\n', stream) != EOF && fflush(stream) == 0;
}
