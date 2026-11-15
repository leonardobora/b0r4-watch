/*
 * aes128.h — a small, dependency-free AES-128 in counter mode.
 *
 * WHY NOT mbedtls: the ESP32 core has mbedtls, but this code has to run and be
 * tested on the build host too (see test/test_mesh/), and the Meshtastic wire
 * format only needs AES-128-CTR. One implementation, testable both places.
 *
 * The counter is incremented over the whole 16-byte block, big-endian, which is
 * what mbedtls_aes_crypt_ctr() does — Meshtastic gets its keystream from mbedtls,
 * so the keystream positions have to line up byte for byte.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {

// One AES-128 block, ECB. Exposed so the known-answer test can check the core
// against FIPS-197 instead of only checking CTR against itself.
void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// AES-128-CTR. `in` and `out` may alias (in-place is the normal case).
// mbedtls semantics: the 16-byte counter starts at `nonce` and increments as a
// big-endian 128-bit integer, wrapping (in practice the wrap never happens for
// a single LoRa payload).
void aes128Ctr(const uint8_t key[16], const uint8_t nonce[16],
               const uint8_t *in, uint8_t *out, size_t len);

}  // namespace mesh
