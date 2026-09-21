/* SHA-256, self-contained (FIPS 180-4). Used for the log's tamper-evident
 * hash chain.
 */
#ifndef LAP_SHA256_H
#define LAP_SHA256_H

#include "lap.h"

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t len);
void sha256_final(Sha256 *c, uint8_t out[32]);
/* One-shot: lowercase hex digest into out[65] (NUL-terminated). */
void sha256_hex(const void *data, size_t len, char out[65]);

#endif /* LAP_SHA256_H */
