// Admin login and sessions.
//
// The password exists on the board only as a PBKDF2-SHA256 hash (from flash.sh).
// Sessions are random 256-bit tokens in an HttpOnly cookie; the board stores only
// their SHA-256, in NVS, so a reboot or OTA doesn't log anyone out. LRU eviction.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "web_internal.h"

static const char *TAG = "web.auth";

#define MAX_SESSIONS 8
#define MAX_ITERS    200000

typedef struct {
    uint8_t hash[32];
    uint32_t seq;  // last use, for LRU; 0 = empty slot
} session_t;

static struct {
    bool have_password;
    uint32_t iters;
    uint8_t salt[32];
    size_t salt_len;
    uint8_t hash[32];
    session_t sess[MAX_SESSIONS];
    uint32_t seq;
    int fails;
    int64_t locked_until_us;
} s;

static void sessions_save(void)
{
    nvs_handle_t h;
    if (nvs_open("web", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "sessions", s.sess, sizeof(s.sess));
    nvs_commit(h);
    nvs_close(h);
}

static void sessions_load(void)
{
    nvs_handle_t h;
    if (nvs_open("web", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s.sess);
    if (nvs_get_blob(h, "sessions", s.sess, &len) != ESP_OK || len != sizeof(s.sess)) {
        memset(s.sess, 0, sizeof(s.sess));
    }
    nvs_close(h);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s.sess[i].seq > s.seq) s.seq = s.sess[i].seq;
    }
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void sha256(const void *in, size_t len, uint8_t out[32])
{
    size_t olen;
    psa_hash_compute(PSA_ALG_SHA_256, in, len, out, 32, &olen);
}

esp_err_t auth_init(const char *admin_hash)
{
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)ps);
        return ESP_FAIL;
    }
    sessions_load();

    // pbkdf2_sha256$<iters>$<salt_b64>$<hash_b64>
    if (!admin_hash || strncmp(admin_hash, "pbkdf2_sha256$", 14) != 0) {
        ESP_LOGW(TAG, "no admin password provisioned: logins are refused (run ./flash.sh)");
        return ESP_OK;
    }
    char buf[128];
    strlcpy(buf, admin_hash + 14, sizeof(buf));
    char *iters = strtok(buf, "$"), *salt = strtok(NULL, "$"), *hash = strtok(NULL, "$");
    if (!iters || !salt || !hash) goto bad;
    s.iters = strtoul(iters, NULL, 10);
    size_t olen;
    if (s.iters < 1000 || s.iters > MAX_ITERS) goto bad;
    if (mbedtls_base64_decode(s.salt, sizeof(s.salt), &s.salt_len, (const uint8_t *)salt,
                              strlen(salt)) != 0 || s.salt_len < 8) goto bad;
    if (mbedtls_base64_decode(s.hash, sizeof(s.hash), &olen, (const uint8_t *)hash,
                              strlen(hash)) != 0 || olen != 32) goto bad;
    s.have_password = true;
    return ESP_OK;
bad:
    ESP_LOGE(TAG, "admin hash malformed: logins are refused");
    return ESP_OK;
}

bool auth_has_password(void)
{
    return s.have_password;
}

static bool password_ok(const char *pw)
{
    uint8_t out[32];
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, s.iters);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, s.salt, s.salt_len);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)pw, strlen(pw));
    if (st == PSA_SUCCESS) st = psa_key_derivation_output_bytes(&op, out, sizeof(out));
    psa_key_derivation_abort(&op);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "pbkdf2 failed: %d", (int)st);
        return false;
    }
    return ct_equal(out, s.hash, sizeof(out));
}

esp_err_t auth_verify(const char *password, int *retry_after_s)
{
    int64_t now = esp_timer_get_time();
    if (now < s.locked_until_us) {
        *retry_after_s = (int)((s.locked_until_us - now) / 1000000) + 1;
        return ESP_ERR_INVALID_STATE;
    }
    if (!s.have_password) return ESP_ERR_NOT_FOUND;

    int64_t t0 = esp_timer_get_time();
    bool ok = password_ok(password);
    ESP_LOGI(TAG, "login %s (pbkdf2 %u iters: %lld ms)", ok ? "ok" : "FAILED",
             (unsigned)s.iters, (esp_timer_get_time() - t0) / 1000);
    if (!ok) {
        // 3 free attempts, then 2, 4, 8 ... 60 s lockouts.
        if (++s.fails >= 3) {
            int lock = 1 << (s.fails - 2);
            if (lock > 60) lock = 60;
            s.locked_until_us = now + (int64_t)lock * 1000000;
        }
        return ESP_FAIL;
    }
    s.fails = 0;
    return ESP_OK;
}

esp_err_t auth_login(const char *password, char token_hex[65], int *retry_after_s)
{
    esp_err_t err = auth_verify(password, retry_after_s);
    if (err != ESP_OK) return err;

    uint8_t token[32];
    esp_fill_random(token, sizeof(token));
    for (int i = 0; i < 32; i++) sprintf(token_hex + 2 * i, "%02x", token[i]);
    token_hex[64] = '\0';

    int slot = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
        if (s.sess[i].seq < s.sess[slot].seq) slot = i;  // empty (0) or least recent
    }
    sha256(token_hex, 64, s.sess[slot].hash);
    s.sess[slot].seq = ++s.seq;
    sessions_save();
    return ESP_OK;
}

static int find_session(httpd_req_t *req)
{
    char tok[80];
    size_t len = sizeof(tok);
    if (httpd_req_get_cookie_val(req, WEB_SESSION_COOKIE, tok, &len) != ESP_OK) return -1;
    if (strlen(tok) != 64) return -1;
    uint8_t h[32];
    sha256(tok, 64, h);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s.sess[i].seq && ct_equal(h, s.sess[i].hash, 32)) return i;
    }
    return -1;
}

bool auth_check_request(httpd_req_t *req)
{
    int i = find_session(req);
    if (i < 0) return false;
    s.sess[i].seq = ++s.seq;  // LRU in RAM; persisted on the next login/logout
    return true;
}

void auth_logout_request(httpd_req_t *req)
{
    int i = find_session(req);
    if (i >= 0) {
        memset(&s.sess[i], 0, sizeof(s.sess[i]));
        sessions_save();
    }
}
