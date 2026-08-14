#ifndef HASH_BRIDGE_H
#define HASH_BRIDGE_H

#include <stdint.h>

/* Double-SHA256 entry point used by the merkle-root generator
 * (src/miner.c) and main.c's --shatest/STAGE4 self-checks. */
void compute_double_sha256(unsigned char *data, unsigned char *hash, int len);

/* SHA-256 midstate: compression state after processing one 64-byte
 * block, without padding/finalization. */
void sha256_midstate(const uint8_t block64[64], uint32_t out_state[8]);

#endif
