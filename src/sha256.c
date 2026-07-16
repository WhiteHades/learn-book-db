#include "learn_book_db/sha256.h"

#include <string.h>

static const uint32_t round_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

static uint32_t rotate_right(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

static uint32_t load_big_endian(const unsigned char *bytes) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void transform(LbdbSha256 *context, const unsigned char block[64]) {
    uint32_t words[64] = {0};
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    uint32_t e = 0;
    uint32_t f = 0;
    uint32_t g = 0;
    uint32_t h = 0;

    for (size_t index = 0; index < 16U; ++index) {
        words[index] = load_big_endian(block + index * 4U);
    }
    for (size_t index = 16U; index < 64U; ++index) {
        const uint32_t first = rotate_right(words[index - 15U], 7U) ^
                               rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
        const uint32_t second = rotate_right(words[index - 2U], 17U) ^
                                rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + first + words[index - 7U] + second;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (size_t index = 0; index < 64U; ++index) {
        const uint32_t sum_one = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ ((~e) & g);
        const uint32_t temporary_one = h + sum_one + choice + round_constants[index] + words[index];
        const uint32_t sum_zero = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary_two = sum_zero + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary_one;
        d = c;
        c = b;
        b = a;
        a = temporary_one + temporary_two;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void lbdb_sha256_init(LbdbSha256 *context) {
    *context = (LbdbSha256){.state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}};
}

void lbdb_sha256_update(LbdbSha256 *context, const void *data, size_t size) {
    const unsigned char *bytes = data;
    if (size > (UINT64_MAX - context->bit_count) / 8U) {
        return;
    }
    context->bit_count += (uint64_t)size * 8U;
    while (size > 0U) {
        const size_t available = 64U - context->block_size;
        const size_t consumed = size < available ? size : available;
        memcpy(context->block + context->block_size, bytes, consumed);
        context->block_size += consumed;
        bytes += consumed;
        size -= consumed;
        if (context->block_size == 64U) {
            transform(context, context->block);
            context->block_size = 0U;
        }
    }
}

void lbdb_sha256_final(LbdbSha256 *context, unsigned char digest[32]) {
    context->block[context->block_size++] = 0x80U;
    if (context->block_size > 56U) {
        while (context->block_size < 64U) {
            context->block[context->block_size++] = 0U;
        }
        transform(context, context->block);
        context->block_size = 0U;
    }
    while (context->block_size < 56U) {
        context->block[context->block_size++] = 0U;
    }
    for (size_t index = 0; index < 8U; ++index) {
        context->block[63U - index] = (unsigned char)(context->bit_count >> (index * 8U));
    }
    transform(context, context->block);
    for (size_t index = 0; index < 8U; ++index) {
        digest[index * 4U] = (unsigned char)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (unsigned char)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (unsigned char)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (unsigned char)context->state[index];
    }
    memset(context, 0, sizeof(*context));
}

void lbdb_sha256_hex(const unsigned char digest[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < 32U; ++index) {
        output[index * 2U] = hex[digest[index] >> 4U];
        output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}
