/*
 * Nonce2/merkle-root work generator:
 *   1. Splice the pool-assigned nonce2 into the coinbase transaction at
 *      the offset the pool specified.
 *   2. Double-SHA256 the resulting coinbase to get the first merkle leaf.
 *   3. Fold in each merkle branch hash supplied by the pool, in order,
 *      via double-SHA256(running_hash || branch_hash).
 *   4. Write the resulting 32-byte root into the block header, with each
 *      4-byte word byte-swapped -- the header stores the root in the
 *      opposite word order from the internal digest layout.
 */

#include <string.h>

#include "util.h"
#include "a3197s_job.h"
#include "hash_bridge.h"

static void store_merkle_root(uint8_t *header_slot, const uint8_t *digest)
{
        const uint32_t *src_words = (const uint32_t *)digest;
        uint32_t *dst_words = (uint32_t *)header_slot;
        int word_count = 32 / 4;
        int w;

        for (w = 0; w < word_count; w++) {
                dst_words[w] = bswap_32(src_words[w]);
        }
}

void bitcoin_build_nonce2_job(a3197s_job_t *mw, uint32_t nonce2)
{
        uint8_t running_hash[32];
        uint8_t fold_buf[64];
        int branch;

        memcpy(mw->coinbase + mw->nonce2_offset, &nonce2, (size_t)mw->nonce2_size);

        compute_double_sha256(mw->coinbase, running_hash, (int)mw->coinbase_len);

        for (branch = 0; branch < mw->nmerkles; branch++) {
                memcpy(fold_buf, running_hash, 32);
                memcpy(fold_buf + 32, mw->merkles[branch], 32);
                compute_double_sha256(fold_buf, running_hash, (int)sizeof(fold_buf));
        }

        store_merkle_root(mw->header + mw->merkle_offset, running_hash);
}
