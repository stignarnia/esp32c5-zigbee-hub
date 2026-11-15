// Noise_NNpsk0_25519_ChaChaPoly_SHA256, responder side: what ESPHome's API speaks
// (esphome/components/noise, built on noise-c). Spec: noiseprotocol.org/noise.html.
//
//   -> psk, e        (client: its ephemeral key, then an encrypted empty payload)
//   <- e, ee         (us: our ephemeral key, encrypted empty payload)
//   split: client sends with k1, we send with k2
//
// X25519 and ChaCha20-Poly1305 come from components/hubcrypto (shared with Tailscale),
// SHA-256 from PSA. Nonces are noise-c's: 4 zero bytes + 64-bit little-endian counter.
#include <string.h>

#include "esp_random.h"
#include "ha_internal.h"
#include "hub_crypto.h"
#include "psa/crypto.h"

static const char PROTOCOL[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

static void sha256_2(uint8_t out[32], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, a, alen);
    if (blen) psa_hash_update(&op, b, blen);
    if (psa_hash_finish(&op, out, 32, &n) != PSA_SUCCESS) memset(out, 0, 32);
}

// HMAC-SHA256 (RFC 2104); the key is always 32 bytes here.
static void hmac(uint8_t out[32], const uint8_t key[32], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    uint8_t pad[64], inner[32];
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n;
    memset(pad, 0x36, sizeof(pad));
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, pad, 64);
    if (alen) psa_hash_update(&op, a, alen);
    if (blen) psa_hash_update(&op, b, blen);
    psa_hash_finish(&op, inner, 32, &n);
    memset(pad, 0x5c, sizeof(pad));
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    sha256_2(out, pad, 64, inner, 32);
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
}

// HKDF(ck, ikm) -> 2 or 3 outputs (Noise §4.3).
static void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len, uint8_t *o1, uint8_t *o2, uint8_t *o3)
{
    uint8_t temp[32], one = 1, two = 2, three = 3, t1[32], t2[32];
    hmac(temp, ck, ikm, ikm_len, NULL, 0);
    hmac(t1, temp, &one, 1, NULL, 0);
    hmac(t2, temp, t1, 32, &two, 1);
    if (o3) hmac(o3, temp, t2, 32, &three, 1);
    memcpy(o1, t1, 32);
    memcpy(o2, t2, 32);
    memset(temp, 0, sizeof(temp));
    memset(t1, 0, sizeof(t1));
    memset(t2, 0, sizeof(t2));
}

static void mix_hash(ha_noise_t *s, const uint8_t *data, size_t len)
{
    sha256_2(s->h, s->h, 32, data, len);
}

static void mix_key(ha_noise_t *s, const uint8_t *ikm, size_t len)
{
    hkdf(s->ck, ikm, len, s->ck, s->k, NULL);
    s->n = 0;
}

static void nonce(uint8_t out[12], uint64_t n)
{
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(n >> (8 * i));
}

void ha_noise_init(ha_noise_t *s, const uint8_t *prologue, size_t plen, const uint8_t psk[32])
{
    memset(s, 0, sizeof(*s));
    // The name is longer than 32 bytes: h = SHA-256(name)
    sha256_2(s->h, (const uint8_t *)PROTOCOL, strlen(PROTOCOL), NULL, 0);
    memcpy(s->ck, s->h, 32);
    mix_hash(s, prologue, plen);
    // psk0: MixKeyAndHash(psk) before the first message
    uint8_t th[32];
    hkdf(s->ck, psk, 32, s->ck, th, s->k);
    mix_hash(s, th, 32);
    s->n = 0;
    memset(th, 0, sizeof(th));
}

bool ha_noise_read_msg1(ha_noise_t *s, const uint8_t *msg, size_t len)
{
    if (len < 32 + HUB_AEAD_TAG || len > 32 + HUB_AEAD_TAG + 64) return false;
    memcpy(s->re, msg, 32);
    mix_hash(s, s->re, 32);
    mix_key(s, s->re, 32);  // the psk modifier mixes e into the key too
    const uint8_t *ct = msg + 32;
    size_t ctlen = len - 32;
    uint8_t n12[12], pt[64];
    nonce(n12, s->n++);
    if (hub_aead_open(s->k, n12, s->h, 32, ct, ctlen, pt) != 0) return false;
    mix_hash(s, ct, ctlen);
    return true;
}

bool ha_noise_write_msg2(ha_noise_t *s, uint8_t out[HA_NOISE_MSG2])
{
    uint8_t e[32], dh[32], re[32];
    esp_fill_random(e, sizeof(e));
    x25519_base(out, e, 1);
    mix_hash(s, out, 32);
    mix_key(s, out, 32);
    memcpy(re, s->re, 32);
    re[31] &= 0x7F;  // RFC 7748: ignore the top bit (this x25519 doesn't)
    bool ok = x25519(dh, e, re, 1) == 0;  // -1: all-zero result (a low-order point)
    memset(e, 0, sizeof(e));
    if (ok) {
        mix_key(s, dh, 32);
        uint8_t n12[12];
        nonce(n12, s->n++);
        ok = hub_aead_seal(s->k, n12, s->h, 32, NULL, 0, out + 32) == 0;
        if (ok) mix_hash(s, out + 32, HUB_AEAD_TAG);
    }
    memset(dh, 0, sizeof(dh));
    if (!ok) return false;
    // Split: the initiator sends with the first key, we send with the second
    hkdf(s->ck, NULL, 0, s->recv_k, s->send_k, NULL);
    s->send_n = s->recv_n = 0;
    memset(s->k, 0, sizeof(s->k));
    memset(s->ck, 0, sizeof(s->ck));
    return true;
}

bool ha_noise_encrypt(ha_noise_t *s, const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t n12[12];
    nonce(n12, s->send_n++);
    return hub_aead_seal(s->send_k, n12, NULL, 0, in, len, out) == 0;
}

bool ha_noise_decrypt(ha_noise_t *s, const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t n12[12];
    nonce(n12, s->recv_n++);
    return hub_aead_open(s->recv_k, n12, NULL, 0, in, len, out) == 0;
}

void ha_noise_wipe(ha_noise_t *s)
{
    memset(s, 0, sizeof(*s));
}
