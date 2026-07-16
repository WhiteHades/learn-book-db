#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char *lbdb_string_duplicate(const char *value) {
    size_t size = 0;
    char *copy = NULL;
    if (value == NULL) {
        return NULL;
    }
    size = strlen(value);
    if (size == SIZE_MAX) {
        return NULL;
    }
    copy = malloc(size + 1U);
    if (copy != NULL) {
        memcpy(copy, value, size + 1U);
    }
    return copy;
}

char *lbdb_string_format(const char *format, ...) {
    va_list arguments;
    va_list copy;
    int needed = 0;
    char *result = NULL;

    va_start(arguments, format);
    va_copy(copy, arguments);
    needed = vsnprintf(NULL, 0U, format, copy);
    va_end(copy);
    if (needed < 0 || (size_t)needed == SIZE_MAX) {
        va_end(arguments);
        return NULL;
    }
    result = malloc((size_t)needed + 1U);
    if (result != NULL) {
        (void)vsnprintf(result, (size_t)needed + 1U, format, arguments);
    }
    va_end(arguments);
    return result;
}

LbdbError lbdb_string_vector_push(LbdbStringVector *vector, const char *value) {
    char **replacement = NULL;
    char *copy = NULL;
    size_t capacity = 0;

    if (vector == NULL || value == NULL) {
        return LBDB_ERROR_INTERNAL;
    }
    if (vector->count == vector->capacity) {
        capacity = vector->capacity == 0U ? 8U : vector->capacity * 2U;
        if (capacity < vector->capacity || capacity > SIZE_MAX / sizeof(*replacement)) {
            return LBDB_ERROR_MEMORY;
        }
        replacement = realloc(vector->items, capacity * sizeof(*replacement));
        if (replacement == NULL) {
            return LBDB_ERROR_MEMORY;
        }
        vector->items = replacement;
        vector->capacity = capacity;
    }
    copy = lbdb_string_duplicate(value);
    if (copy == NULL) {
        return LBDB_ERROR_MEMORY;
    }
    vector->items[vector->count++] = copy;
    return LBDB_OK;
}

void lbdb_string_vector_destroy(LbdbStringVector *vector) {
    if (vector == NULL) {
        return;
    }
    for (size_t index = 0; index < vector->count; ++index) {
        free(vector->items[index]);
    }
    free(vector->items);
    *vector = (LbdbStringVector){0};
}

static LbdbError normalize_absolute_path(LbdbApp *app, const char *input, char **output) {
    char *working = NULL;
    char **segments = NULL;
    size_t segment_count = 0U;
    size_t segment_capacity = 0U;
    char *save = NULL;
    char *token = NULL;
    size_t output_size = 2U;
    char *normalized = NULL;
    char *cursor = NULL;

    if (input[0] != '/') {
        (void)lbdb_app_fail(app, LBDB_ERROR_INTERNAL,
                            "Path normalization requires an absolute path");
        return LBDB_ERROR_INTERNAL;
    }
    working = lbdb_string_duplicate(input);
    if (working == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate path buffer");
    }
    token = strtok_r(working, "/", &save);
    while (token != NULL) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/", &save);
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (segment_count > 0U) {
                segment_count -= 1U;
            }
            token = strtok_r(NULL, "/", &save);
            continue;
        }
        if (segment_count == segment_capacity) {
            const size_t next_capacity = segment_capacity == 0U ? 8U : segment_capacity * 2U;
            char **replacement = NULL;
            if (next_capacity < segment_capacity || next_capacity > SIZE_MAX / sizeof(*segments)) {
                free(segments);
                free(working);
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Path has too many components");
            }
            replacement = realloc(segments, next_capacity * sizeof(*segments));
            if (replacement == NULL) {
                free(segments);
                free(working);
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate path components");
            }
            segments = replacement;
            segment_capacity = next_capacity;
        }
        segments[segment_count++] = token;
        if (strlen(token) > SIZE_MAX - output_size - 1U) {
            free(segments);
            free(working);
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Resolved path is too long");
        }
        output_size += strlen(token) + 1U;
        token = strtok_r(NULL, "/", &save);
    }
    normalized = malloc(output_size);
    if (normalized == NULL) {
        free(segments);
        free(working);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate resolved path");
    }
    cursor = normalized;
    *cursor++ = '/';
    for (size_t index = 0; index < segment_count; ++index) {
        const size_t length = strlen(segments[index]);
        memcpy(cursor, segments[index], length);
        cursor += length;
        if (index + 1U < segment_count) {
            *cursor++ = '/';
        }
    }
    *cursor = '\0';
    free(segments);
    free(working);
    *output = normalized;
    return LBDB_OK;
}

static bool path_is_within(const char *root, const char *path) {
    const size_t root_size = strlen(root);
    if (strcmp(root, "/") == 0) {
        return path[0] == '/';
    }
    return strncmp(root, path, root_size) == 0 &&
           (path[root_size] == '\0' || path[root_size] == '/');
}

LbdbError lbdb_resolve_path(LbdbApp *app, const char *value, bool must_exist,
                            bool require_within_root, char **resolved) {
    char *joined = NULL;
    char *normalized = NULL;
    char *canonical = NULL;

    if (app == NULL || value == NULL || value[0] == '\0' || resolved == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Path must not be empty");
    }
    *resolved = NULL;
    if (value[0] == '/') {
        joined = lbdb_string_duplicate(value);
    } else {
        joined = lbdb_string_format("%s/%s", app->root, value);
    }
    if (joined == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate path");
    }
    LbdbError error = normalize_absolute_path(app, joined, &normalized);
    free(joined);
    if (error != LBDB_OK) {
        return error;
    }
    if (normalized == NULL) {
        (void)lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Path normalization produced no result");
        return LBDB_ERROR_INTERNAL;
    }
    if (must_exist) {
        canonical = realpath(normalized, NULL);
        if (canonical == NULL) {
            error = lbdb_app_fail(app, errno == ENOENT ? LBDB_ERROR_NOT_FOUND : LBDB_ERROR_IO,
                                  "Cannot resolve path %s: %s", normalized, strerror(errno));
            free(normalized);
            return error;
        }
        free(normalized);
        normalized = canonical;
    }
    if (require_within_root && !path_is_within(app->root, normalized)) {
        error = lbdb_app_fail(app, LBDB_ERROR_VALIDATION, "Path escapes project root: %s", value);
        free(normalized);
        return error;
    }
    *resolved = normalized;
    return LBDB_OK;
}

LbdbError lbdb_make_parent_directories(LbdbApp *app, const char *path) {
    char *copy = lbdb_string_duplicate(path);
    char *separator = NULL;
    if (copy == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate directory path");
    }
    separator = strrchr(copy, '/');
    if (separator == NULL || separator == copy) {
        free(copy);
        return LBDB_OK;
    }
    *separator = '\0';
    for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            LbdbError error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot create directory %s: %s",
                                            copy, strerror(errno));
            free(copy);
            return error;
        }
        *cursor = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        LbdbError error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot create directory %s: %s", copy,
                                        strerror(errno));
        free(copy);
        return error;
    }
    free(copy);
    return LBDB_OK;
}

LbdbError lbdb_read_file(LbdbApp *app, const char *path, char **contents, size_t *size) {
    FILE *stream = NULL;
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 8192U;

    if (contents == NULL || size == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_INTERNAL, "Invalid file reader output");
    }
    *contents = NULL;
    *size = 0U;
    stream = fopen(path, "rb");
    if (stream == NULL) {
        return lbdb_app_fail(app, errno == ENOENT ? LBDB_ERROR_NOT_FOUND : LBDB_ERROR_IO,
                             "Cannot open %s: %s", path, strerror(errno));
    }
    buffer = malloc(capacity + 1U);
    if (buffer == NULL) {
        fclose(stream);
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate file buffer");
    }
    while (!feof(stream)) {
        size_t read_size = 0U;
        if (length == capacity) {
            char *replacement = NULL;
            if (capacity > (SIZE_MAX - 1U) / 2U) {
                free(buffer);
                fclose(stream);
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "File is too large: %s", path);
            }
            capacity *= 2U;
            replacement = realloc(buffer, capacity + 1U);
            if (replacement == NULL) {
                free(buffer);
                fclose(stream);
                return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to grow file buffer");
            }
            buffer = replacement;
        }
        read_size = fread(buffer + length, 1U, capacity - length, stream);
        if (read_size > SIZE_MAX - length) {
            free(buffer);
            fclose(stream);
            return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "File size overflow: %s", path);
        }
        length += read_size;
        if (ferror(stream)) {
            LbdbError error =
                lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot read %s: %s", path, strerror(errno));
            free(buffer);
            fclose(stream);
            return error;
        }
    }
    if (fclose(stream) != 0) {
        LbdbError error =
            lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot close %s: %s", path, strerror(errno));
        free(buffer);
        return error;
    }
    buffer[length] = '\0';
    *contents = buffer;
    *size = length;
    return LBDB_OK;
}

LbdbError lbdb_write_file_exclusive(LbdbApp *app, const char *path, const void *contents,
                                    size_t size) {
    int descriptor = -1;
    FILE *stream = NULL;
    LbdbError error = LBDB_OK;

    LBDB_TRY(lbdb_make_parent_directories(app, path));
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) {
        return lbdb_app_fail(app, errno == EEXIST ? LBDB_ERROR_CONFLICT : LBDB_ERROR_IO,
                             "Cannot create %s: %s", path, strerror(errno));
    }
    stream = fdopen(descriptor, "wb");
    if (stream == NULL) {
        close(descriptor);
        unlink(path);
        return lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot open stream for %s: %s", path,
                             strerror(errno));
    }
    if (size > 0U && fwrite(contents, 1U, size, stream) != size) {
        error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot write %s: %s", path, strerror(errno));
    } else if (fflush(stream) != 0 || fsync(descriptor) != 0) {
        error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot flush %s: %s", path, strerror(errno));
    }
    if (fclose(stream) != 0 && error == LBDB_OK) {
        error = lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot close %s: %s", path, strerror(errno));
    }
    if (error != LBDB_OK) {
        unlink(path);
    }
    return error;
}

LbdbError lbdb_write_json_file_exclusive(LbdbApp *app, const char *path,
                                         const LbdbJsonWriter *writer) {
    const size_t size = lbdb_json_size(writer);
    char *contents = NULL;
    LbdbError error = LBDB_OK;
    if (size == SIZE_MAX) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "JSON output is too large");
    }
    contents = malloc(size + 1U);
    if (contents == NULL) {
        return lbdb_app_fail(app, LBDB_ERROR_MEMORY, "Unable to allocate JSON file output");
    }
    memcpy(contents, lbdb_json_data(writer), size);
    contents[size] = '\n';
    error = lbdb_write_file_exclusive(app, path, contents, size + 1U);
    free(contents);
    return error;
}

LbdbError lbdb_file_sha256(LbdbApp *app, const char *path, char output[65]) {
    FILE *stream = fopen(path, "rb");
    unsigned char buffer[16384] = {0};
    unsigned char digest[32] = {0};
    LbdbSha256 context = {0};
    if (stream == NULL) {
        return lbdb_app_fail(app, errno == ENOENT ? LBDB_ERROR_NOT_FOUND : LBDB_ERROR_IO,
                             "Cannot open %s: %s", path, strerror(errno));
    }
    lbdb_sha256_init(&context);
    while (!feof(stream)) {
        const size_t size = fread(buffer, 1U, sizeof(buffer), stream);
        lbdb_sha256_update(&context, buffer, size);
        if (ferror(stream)) {
            LbdbError error =
                lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot hash %s: %s", path, strerror(errno));
            fclose(stream);
            return error;
        }
    }
    if (fclose(stream) != 0) {
        return lbdb_app_fail(app, LBDB_ERROR_IO, "Cannot close %s: %s", path, strerror(errno));
    }
    lbdb_sha256_final(&context, digest);
    lbdb_sha256_hex(digest, output);
    return LBDB_OK;
}

bool lbdb_path_exists(const char *path) {
    struct stat status = {0};
    return path != NULL && stat(path, &status) == 0;
}

bool lbdb_path_is_file(const char *path) {
    struct stat status = {0};
    return path != NULL && stat(path, &status) == 0 && S_ISREG(status.st_mode);
}
