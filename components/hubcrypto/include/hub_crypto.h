// Crypto shared by Tailscale (microlink's Noise IK, WireGuard) and the Home Assistant API
// (ESPHome's Noise NNpsk0): X25519 (x25519.h, STROBE reference code, MIT) and
// ChaCha20-Poly1305 through PSA (mbedTLS 4 made its chachapoly module private).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "x25519.h"

#define HUB_AEAD_TAG 16

// out = ciphertext (len bytes) + tag (16). Keys are imported for the one call only.
// 0 on success, -1 on failure.
int hub_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t len, uint8_t *out);

// in = ciphertext + tag (len >= 16); out gets len - 16 bytes. -1 if the tag doesn't match.
int hub_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t len, uint8_t *out);
