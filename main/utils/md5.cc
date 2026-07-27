#include "md5.h"

#include <cstring>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <esp_log.h>

#define TAG "MD5"

#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define FF(a, b, c, d, x, s, ac) { \
    (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define GG(a, b, c, d, x, s, ac) { \
    (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define HH(a, b, c, d, x, s, ac) { \
    (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}
#define II(a, b, c, d, x, s, ac) { \
    (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTATE_LEFT((a), (s)); \
    (a) += (b); \
}

// 常量表（标准 MD5）
static const uint32_t kMD5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static const uint32_t kMD5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

void MD5::Init(Context* ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void MD5::Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i * 4]
             | ((uint32_t)block[i * 4 + 1] << 8)
             | ((uint32_t)block[i * 4 + 2] << 16)
             | ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = F(b, c, d);
            g = i;
        } else if (i < 32) {
            f = G(b, c, d);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = H(b, c, d);
            g = (3 * i + 5) % 16;
        } else {
            f = I(b, c, d);
            g = (7 * i) % 16;
        }
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + ROTATE_LEFT((a + f + kMD5K[i] + x[g]), kMD5S[i]);
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void MD5::Update(Context* ctx, const uint8_t* input, size_t inputLen) {
    uint32_t i, idx, partLen;

    idx = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    if ((ctx->count[0] += (uint32_t)inputLen << 3) < ((uint32_t)inputLen << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)inputLen >> 29;
    partLen = 64 - idx;

    if (inputLen >= partLen) {
        memcpy(&ctx->buffer[idx], input, partLen);
        Transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64) {
            Transform(ctx->state, &input[i]);
        }
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[idx], &input[i], inputLen - i);
}

void MD5::Final(uint8_t digest[16], Context* ctx) {
    uint8_t bits[8];
    for (int i = 0; i < 8; i++) {
        bits[i] = (uint8_t)((ctx->count[(i >= 4) ? 1 : 0] >> ((i % 4) * 8 + (i >= 4 ? 0 : 0))) & 0xFF);
    }
    uint32_t lo = ctx->count[0];
    uint32_t hi = ctx->count[1];
    for (int i = 0; i < 4; i++) {
        bits[i]     = (uint8_t)(lo >> (i * 8));
        bits[i + 4] = (uint8_t)(hi >> (i * 8));
    }

    uint32_t idx = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    uint32_t padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    uint8_t pad[64] = {0x80};
    Update(ctx, pad, padLen);
    Update(ctx, bits, 8);

    for (int i = 0; i < 4; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i]);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

std::string MD5::ToHexString(const uint8_t digest[16]) {
    char hex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(hex + i * 2, "%02x", digest[i]);
    }
    hex[32] = '\0';
    return std::string(hex);
}

std::string MD5::Calculate(const uint8_t* data, size_t len) {
    Context ctx;
    Init(&ctx);
    Update(&ctx, data, len);
    uint8_t digest[16];
    Final(digest, &ctx);
    return ToHexString(digest);
}

std::string MD5::Calculate(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path.c_str());
        return std::string();
    }

    Context ctx;
    Init(&ctx);

    const size_t BUFFER_SIZE = 4096;
    uint8_t buffer[BUFFER_SIZE];

    while (file.read(reinterpret_cast<char*>(buffer), BUFFER_SIZE) || file.gcount() > 0) {
        Update(&ctx, buffer, static_cast<size_t>(file.gcount()));
    }

    file.close();

    uint8_t digest[16];
    Final(digest, &ctx);
    return ToHexString(digest);
}
