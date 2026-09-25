#include "sha256.h"
#include "test.h"

#include <stdlib.h>

void test_sha256(void) {
    char hex[65];

    t_begin("sha256: FIPS 180-4 vectors");
    sha256_hex("", 0, hex);
    ASSERT_EQ_S(
        hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256_hex("abc", 3, hex);
    ASSERT_EQ_S(
        hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
               hex);
    ASSERT_EQ_S(
        hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    t_begin("sha256: one million 'a'");
    char *m = (char *)malloc(1000000);
    memset(m, 'a', 1000000);
    sha256_hex(m, 1000000, hex);
    free(m);
    ASSERT_EQ_S(
        hex,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    t_begin("sha256: streaming equals one-shot across block boundaries");
    /* 200 bytes fed in odd-sized pieces must match the one-shot digest */
    uint8_t data[200];
    for (int32_t i = 0; i < 200; i++)
        data[i] = (uint8_t)(i * 7 + 3);
    char oneshot[65];
    sha256_hex(data, 200, oneshot);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, 1);
    sha256_update(&c, data + 1, 63);
    sha256_update(&c, data + 64, 64);
    sha256_update(&c, data + 128, 5);
    sha256_update(&c, data + 133, 67);
    uint8_t digest[32];
    sha256_final(&c, digest);
    char streamed[65];
    static const char hexd[] = "0123456789abcdef";
    for (int32_t i = 0; i < 32; i++) {
        streamed[i * 2] = hexd[digest[i] >> 4];
        streamed[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    streamed[64] = '\0';
    ASSERT_EQ_S(streamed, oneshot);

    t_begin("sha256: 55/56/64-byte padding edges");
    /* lengths around the padding boundary must not crash and must differ */
    char h55[65], h56[65], h64[65];
    uint8_t buf[64];
    memset(buf, 'x', sizeof buf);
    sha256_hex(buf, 55, h55);
    sha256_hex(buf, 56, h56);
    sha256_hex(buf, 64, h64);
    ASSERT_TRUE(strcmp(h55, h56) != 0);
    ASSERT_TRUE(strcmp(h56, h64) != 0);
}
