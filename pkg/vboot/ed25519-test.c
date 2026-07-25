/* ed25519-test.c — known-answer tests for ed25519.h.
 *
 * Crypto that is "probably right" is worse than none, so this checks the
 * implementation against the RFC 8032 section 7.1 test vectors plus a SHA-512
 * vector, and confirms that tampering is actually rejected. Run it on the host
 * before trusting the code to decide whether a kernel boots:
 *
 *      cc -O2 -o /tmp/edtest pkg/vboot/ed25519-test.c && /tmp/edtest
 */
#include <stdio.h>
#include <string.h>
#include "ed25519.h"

static int fails;

static void hex2bin(const char *h, ed_u8 *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = h[2*i], lo = h[2*i+1];
        hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
        lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
        out[i] = (ed_u8)((hi << 4) | lo);
    }
}
static void check(const char *what, int ok) {
    printf("  %-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}
static int same(const ed_u8 *a, const char *hex, int n) {
    ed_u8 want[64];
    hex2bin(hex, want, n);
    return memcmp(a, want, n) == 0;
}

/* RFC 8032 7.1 */
struct vec { const char *seed, *pub, *msg, *sig; int msglen; };
static const struct vec V[] = {
    { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 0 },
    { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", 1 },
    { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a", 2 },
};

int main(void) {
    printf("SHA-512\n");
    {   /* NIST: SHA-512("abc") */
        ed_u8 out[64];
        ed_sha512((const ed_u8 *)"abc", 3, out);
        check("SHA-512(\"abc\")",
              same(out, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea2"
                        "0a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
                        "454d4423643ce80e2a9ac94fa54ca49f", 64));
    }

    printf("Ed25519 (RFC 8032 test vectors)\n");
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        ed_u8 seed[32], pub[32], msg[32], sig[64], got[64];
        hex2bin(V[i].seed, seed, 32);
        hex2bin(V[i].pub, pub, 32);
        if (V[i].msglen) hex2bin(V[i].msg, msg, V[i].msglen);
        hex2bin(V[i].sig, sig, 64);

        ed_u8 mypub[32];
        ed25519_pubkey(mypub, seed);
        char label[64];
        snprintf(label, sizeof label, "vector %u: public key derived", i + 1);
        check(label, memcmp(mypub, pub, 32) == 0);

        ed25519_sign(got, msg, V[i].msglen, seed, pub);
        snprintf(label, sizeof label, "vector %u: signature matches", i + 1);
        check(label, memcmp(got, sig, 64) == 0);

        snprintf(label, sizeof label, "vector %u: verifies", i + 1);
        check(label, ed25519_verify(sig, msg, V[i].msglen, pub) == 0);
    }

    printf("rejection\n");
    {
        ed_u8 seed[32], pub[32], sig[64];
        ed_u8 msg[64];
        for (int i = 0; i < 64; i++) msg[i] = (ed_u8)i;
        hex2bin(V[0].seed, seed, 32);
        ed25519_pubkey(pub, seed);
        ed25519_sign(sig, msg, sizeof msg, seed, pub);
        check("a good signature verifies", ed25519_verify(sig, msg, sizeof msg, pub) == 0);

        msg[7] ^= 1;
        check("a modified message is rejected", ed25519_verify(sig, msg, sizeof msg, pub) != 0);
        msg[7] ^= 1;
        sig[3] ^= 1;
        check("a modified signature is rejected", ed25519_verify(sig, msg, sizeof msg, pub) != 0);
        sig[3] ^= 1;
        pub[9] ^= 1;
        check("the wrong public key is rejected", ed25519_verify(sig, msg, sizeof msg, pub) != 0);
    }

    printf("\n%s\n", fails ? "SOME TESTS FAILED — do not ship this" : "all tests passed");
    return fails != 0;
}
