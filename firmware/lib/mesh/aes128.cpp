#include "aes128.h"

#include <string.h>

namespace mesh {
namespace {

// The S-box is DERIVED, not pasted: a 256-entry table typed from memory is a
// silent-corruption risk, and the derivation is 4 lines. FIPS-197: the inverse
// in GF(2^8) modulo 0x11b, then the affine transform.
inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) ? 0x1b : 0x00)); }

uint8_t gfMul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b = (uint8_t)(b >> 1);
    }
    return p;
}

uint8_t gfInv(uint8_t a)
{
    if (a == 0) return 0;
    for (int b = 1; b < 256; b++)
        if (gfMul(a, (uint8_t)b) == 1) return (uint8_t)b;
    return 0;   // unreachable: every non-zero element has an inverse
}

inline uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

const uint8_t *sbox()
{
    static uint8_t s[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            const uint8_t inv = gfInv((uint8_t)i);
            s[i] = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                             rotl8(inv, 4) ^ 0x63);
        }
        built = true;
    }
    return s;
}

struct Aes128 {
    uint8_t rk[176];   // 11 round keys

    void expand(const uint8_t key[16])
    {
        memcpy(rk, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4];
            memcpy(t, rk + i - 4, 4);
            if (i % 16 == 0) {
                const uint8_t tmp = t[0];
                t[0] = (uint8_t)(sbox()[t[1]] ^ rcon);
                t[1] = sbox()[t[2]];
                t[2] = sbox()[t[3]];
                t[3] = sbox()[tmp];
                rcon = xtime(rcon);
            }
            for (int k = 0; k < 4; k++) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
        }
    }

    // State is column-major, matching the spec's byte order: byte i of the
    // block goes to row i%4, column i/4.
    void encrypt(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);

        for (int round = 1; round <= 10; round++) {
            uint8_t t[16];
            // SubBytes + ShiftRows
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    t[4 * c + r] = sbox()[s[4 * ((c + r) & 3) + r]];
                }
            }
            // MixColumns (skipped in the last round)
            if (round != 10) {
                for (int c = 0; c < 4; c++) {
                    const uint8_t *a = t + 4 * c;
                    const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    s[4 * c + 0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                    s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                    s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                    s[4 * c + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
                }
            } else {
                memcpy(s, t, 16);
            }
            // AddRoundKey
            for (int i = 0; i < 16; i++) s[i] = (uint8_t)(s[i] ^ rk[round * 16 + i]);
        }
        memcpy(out, s, 16);
    }
};

}   // namespace

void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    Aes128 c;
    c.expand(key);
    c.encrypt(in, out);
}

void aes128Ctr(const uint8_t key[16], const uint8_t nonce[16],
               const uint8_t *in, uint8_t *out, size_t len)
{
    Aes128 c;
    c.expand(key);

    uint8_t ctr[16];
    memcpy(ctr, nonce, 16);

    for (size_t i = 0; i < len; i += 16) {
        uint8_t ks[16];
        c.encrypt(ctr, ks);

        // mbedtls increments the whole 16-byte counter as a big-endian integer.
        for (int j = 15; j >= 0; j--) {
            if (++ctr[j] != 0) break;
        }

        const size_t n = (len - i < 16) ? (len - i) : 16;
        for (size_t k = 0; k < n; k++) out[i + k] = (uint8_t)(in[i + k] ^ ks[k]);
    }
}

}  // namespace mesh
