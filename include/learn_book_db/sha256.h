#ifndef LEARN_BOOK_DB_SHA256_H
#define LEARN_BOOK_DB_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct LbdbSha256 {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_size;
} LbdbSha256;

void lbdb_sha256_init(LbdbSha256 *context);
void lbdb_sha256_update(LbdbSha256 *context, const void *data, size_t size);
void lbdb_sha256_final(LbdbSha256 *context, unsigned char digest[32]);
void lbdb_sha256_hex(const unsigned char digest[32], char output[65]);

#endif
