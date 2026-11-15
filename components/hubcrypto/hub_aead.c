// ChaCha20-Poly1305 (RFC 8439) through PSA. Nonce layout is the caller's: Noise
// implementations differ (Tailscale: big-endian counter, noise-c/ESPHome: little-endian).
#include "hub_crypto.h"

#include "psa/crypto.h"

static psa_status_t aead_key(const uint8_t *key, psa_key_usage_t usage, psa_key_id_t *id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, PSA_ALG_CHACHA20_POLY1305);
    psa_status_t st = psa_import_key(&attr, key, 32, id);
    psa_reset_key_attributes(&attr);
    return st;
}

int hub_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t len, uint8_t *out)
{
    psa_key_id_t id;
    if (aead_key(key, PSA_KEY_USAGE_ENCRYPT, &id) != PSA_SUCCESS) return -1;
    size_t out_len = 0;
    psa_status_t st = psa_aead_encrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, 12, ad, ad_len, in, len, out,
                                       len + HUB_AEAD_TAG, &out_len);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && out_len == len + HUB_AEAD_TAG) ? 0 : -1;
}

int hub_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_len,
                  const uint8_t *in, size_t len, uint8_t *out)
{
    if (len < HUB_AEAD_TAG) return -1;
    psa_key_id_t id;
    if (aead_key(key, PSA_KEY_USAGE_DECRYPT, &id) != PSA_SUCCESS) return -1;
    size_t out_len = 0;
    psa_status_t st = psa_aead_decrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, 12, ad, ad_len, in, len, out,
                                       len - HUB_AEAD_TAG, &out_len);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && out_len == len - HUB_AEAD_TAG) ? 0 : -1;
}
