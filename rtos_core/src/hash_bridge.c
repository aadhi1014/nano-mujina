/*
 * Software double-SHA256 (FIPS 180-4). Implements the standard message
 * schedule, compression round, and padding, plus a midstate function
 * that runs one 64-byte block's compression round without padding: since
 * a block header's first 64 bytes (version+prevhash+most of
 * merkle_root) don't change as nonce varies, the compression state after
 * that block can be computed once per job and reused.
 */

#include <string.h>

#include "hash_bridge.h"

static const uint32_t k_round_constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr32(uint32_t x, int n)
{
        return (x >> n) | (x << (32 - n));
}

/* Consumes exactly one 64-byte block, updating `state` (8 uint32_t's,
 * the standard SHA-256 internal representation) in place. */
static void compress_one_block(uint32_t state[8], const uint8_t block[64])
{
        uint32_t schedule[64];
        uint32_t a, b, c, d, e, f, g, h;
        int t;

        for (t = 0; t < 16; t++) {
                schedule[t] = ((uint32_t)block[t * 4] << 24) |
                              ((uint32_t)block[t * 4 + 1] << 16) |
                              ((uint32_t)block[t * 4 + 2] << 8) |
                              (uint32_t)block[t * 4 + 3];
        }
        for (t = 16; t < 64; t++) {
                uint32_t s0 = rotr32(schedule[t - 15], 7) ^ rotr32(schedule[t - 15], 18) ^ (schedule[t - 15] >> 3);
                uint32_t s1 = rotr32(schedule[t - 2], 17) ^ rotr32(schedule[t - 2], 19) ^ (schedule[t - 2] >> 10);

                schedule[t] = schedule[t - 16] + s0 + schedule[t - 7] + s1;
        }

        a = state[0]; b = state[1]; c = state[2]; d = state[3];
        e = state[4]; f = state[5]; g = state[6]; h = state[7];

        for (t = 0; t < 64; t++) {
                uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
                uint32_t ch = (e & f) ^ ((~e) & g);
                uint32_t temp1 = h + s1 + ch + k_round_constants[t] + schedule[t];
                uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = s0 + maj;

                h = g; g = f; f = e; e = d + temp1;
                d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static const uint32_t sha256_iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

void sha256_midstate(const uint8_t block64[64], uint32_t out_state[8])
{
        memcpy(out_state, sha256_iv, sizeof(sha256_iv));
        compress_one_block(out_state, block64);
}

/* Full FIPS 180-4 SHA-256: standard padding (0x80, zero-fill to 56 mod
 * 64, 8-byte big-endian bit length) then one or two final compression
 * blocks beyond the full-block loop. A fixed 128-byte stack buffer holds
 * the padding regardless of input length. */
static void sha256_full(const uint8_t *data, uint32_t len, uint8_t digest[32])
{
        uint32_t state[8];
        uint32_t full_blocks = len / 64;
        uint32_t remainder = len % 64;
        uint32_t tail_len;
        uint64_t bit_len = (uint64_t)len * 8;
        uint8_t tail[128];
        uint32_t i;

        memcpy(state, sha256_iv, sizeof(state));

        for (i = 0; i < full_blocks; i++)
                compress_one_block(state, data + i * 64);

        memset(tail, 0, sizeof(tail));
        memcpy(tail, data + full_blocks * 64, remainder);
        tail[remainder] = 0x80;
        tail_len = (remainder < 56) ? 64 : 128;
        for (i = 0; i < 8; i++)
                tail[tail_len - 1 - i] = (uint8_t)(bit_len >> (8 * i));

        compress_one_block(state, tail);
        if (tail_len == 128)
                compress_one_block(state, tail + 64);

        for (i = 0; i < 8; i++) {
                digest[i * 4 + 0] = (uint8_t)(state[i] >> 24);
                digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
                digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
                digest[i * 4 + 3] = (uint8_t)(state[i] >> 0);
        }
}

void compute_double_sha256(unsigned char *data, unsigned char *hash, int len)
{
        uint8_t first[32];

        sha256_full((const uint8_t *)data, (uint32_t)len, first);
        sha256_full(first, 32, hash);
}
