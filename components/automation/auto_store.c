// Automation storage: script files and their metadata on LittleFS, secrets and the time
// zone in NVS, script `state` (state.get/put) as one JSON file per script.
//
// Who writes what: scripts/metadata/secrets/time zone — the HTTP server task (API
// calls); state files — the auto_io task, the reboot gate and the shutdown handler.
// The automation task itself never touches flash (its stack is in PSRAM).
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "auto_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "auto";

#define SECRETS_NS "secrets"
#define AUTO_NS    "auto"

// ---- files ---------------------------------------------------------------------------------

static esp_err_t write_file(const char *path, const char *data, size_t len)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "write %s: %s", tmp, strerror(errno));
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "write %s failed", path);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n > max) {
        fclose(f);
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        if (len) *len = (size_t)n;
    }
    return buf;
}

void store_init_dirs(void)
{
    mkdir(AUTO_DIR, 0775);
    mkdir(AUTO_STATE_DIR, 0775);
    mkdir(AUTO_SCRIPT_DIR, 0775);
}

bool store_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > AUTO_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static void script_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, AUTO_SCRIPT_DIR "/%s.lua", name);
}

char *store_read_script(const char *name, size_t *len)
{
    char path[64];
    script_path(path, sizeof(path), name);
    return read_file(path, AUTO_SCRIPT_MAX, len);
}

esp_err_t store_write_script(const char *name, const char *src, size_t len)
{
    char path[64];
    script_path(path, sizeof(path), name);
    return write_file(path, src, len);
}

void store_delete_script(const char *name)
{
    char path[64];
    script_path(path, sizeof(path), name);
    unlink(path);
}

// Metadata: which scripts exist and which are enabled. Script files without an entry
// (copied in by hand) show up disabled.
esp_err_t store_load_meta(void)
{
    size_t len;
    char *txt = read_file(AUTO_META, 16 * 1024, &len);
    cJSON *j = txt ? cJSON_ParseWithLength(txt, len) : NULL;
    free(txt);
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "scripts")) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        if (!name || !store_valid_name(name) || n >= AUTO_MAX_SCRIPTS) continue;
        script_t *s = &g_scripts[n++];
        s->used = true;
        strlcpy(s->name, name, sizeof(s->name));
        s->enabled = cJSON_IsTrue(cJSON_GetObjectItem(e, "enabled"));
    }
    cJSON_Delete(j);
    DIR *d = opendir(AUTO_SCRIPT_DIR);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL && n < AUTO_MAX_SCRIPTS) {
        char name[AUTO_NAME_MAX + 8];
        strlcpy(name, de->d_name, sizeof(name));
        char *dot = strrchr(name, '.');
        if (!dot || strcmp(dot, ".lua") != 0) continue;
        *dot = '\0';
        if (!store_valid_name(name)) continue;
        bool known = false;
        for (int i = 0; i < n && !known; i++) known = strcmp(g_scripts[i].name, name) == 0;
        if (known) continue;
        script_t *s = &g_scripts[n++];
        s->used = true;
        strlcpy(s->name, name, sizeof(s->name));
        s->enabled = false;
    }
    if (d) closedir(d);
    ESP_LOGI(TAG, "%d script(s) on flash", n);
    return ESP_OK;
}

esp_err_t store_write_meta(void)
{
    static SemaphoreHandle_t s_meta_mtx;  // one writer at a time (created on first use: HTTP task only)
    if (!s_meta_mtx) s_meta_mtx = xSemaphoreCreateMutex();
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "scripts");
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        if (!g_scripts[i].used) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", g_scripts[i].name);
        cJSON_AddBoolToObject(e, "enabled", g_scripts[i].enabled);
        cJSON_AddItemToArray(arr, e);
    }
    xSemaphoreGive(g_mtx);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_meta_mtx, portMAX_DELAY);
    esp_err_t r = write_file(AUTO_META, txt, strlen(txt));
    xSemaphoreGive(s_meta_mtx);
    cJSON_free(txt);
    return r;
}

// ---- secrets (NVS, RAM copy in PSRAM) -----------------------------------------------------------

typedef struct {
    char name[AUTO_SECRET_NAME + 1];
    char *value;
} secret_t;

static secret_t s_secrets[AUTO_MAX_SECRETS];

static bool valid_secret_name(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > AUTO_SECRET_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        if (!(islower((unsigned char)n[i]) || isdigit((unsigned char)n[i]) || n[i] == '_')) return false;
    }
    return true;
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *c = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) memcpy(c, s, n);
    return c;
}

static void secret_free(secret_t *s)
{
    if (s->value) {
        memset(s->value, 0, strlen(s->value));
        free(s->value);
    }
    memset(s, 0, sizeof(*s));
}

void store_secrets_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SECRETS_NS, NVS_READONLY, &h) != ESP_OK) return;  // none yet
    nvs_iterator_t it = NULL;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, SECRETS_NS, NVS_TYPE_STR, &it);
    int n = 0;
    while (r == ESP_OK && n < AUTO_MAX_SECRETS) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char val[AUTO_SECRET_MAX + 1];
        size_t len = sizeof(val);
        if (nvs_get_str(h, info.key, val, &len) == ESP_OK) {
            strlcpy(s_secrets[n].name, info.key, sizeof(s_secrets[n].name));
            s_secrets[n].value = psram_strdup(val);
            if (s_secrets[n].value) n++;
        }
        memset(val, 0, sizeof(val));
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    ESP_LOGI(TAG, "%d secret(s)", n);
}

esp_err_t store_secret_set(const char *name, const char *value)
{
    if (!valid_secret_name(name)) return ESP_ERR_INVALID_ARG;
    if (value && (strlen(value) == 0 || strlen(value) > AUTO_SECRET_MAX)) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    esp_err_t r = nvs_open(SECRETS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = value ? nvs_set_str(h, name, value) : nvs_erase_key(h, name);
    if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK;
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) return r;
    char *copy = value ? psram_strdup(value) : NULL;
    if (value && !copy) return ESP_ERR_NO_MEM;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < AUTO_MAX_SECRETS; i++) {
        if (s_secrets[i].value && strcmp(s_secrets[i].name, name) == 0) slot = i;
        if (!s_secrets[i].value && free_slot < 0) free_slot = i;
    }
    if (slot >= 0) secret_free(&s_secrets[slot]);
    else slot = free_slot;
    if (copy && slot >= 0) {
        strlcpy(s_secrets[slot].name, name, sizeof(s_secrets[slot].name));
        s_secrets[slot].value = copy;
        copy = NULL;
    }
    xSemaphoreGive(g_mtx);
    if (copy) {  // no free slot (NVS has it, RAM doesn't): refuse rather than half-apply
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "secret '%s' %s", name, value ? "set" : "deleted");
    return ESP_OK;
}

const char *store_secret_get(const char *name)
{
    for (int i = 0; i < AUTO_MAX_SECRETS; i++) {
        if (s_secrets[i].value && strcmp(s_secrets[i].name, name) == 0) return s_secrets[i].value;
    }
    return NULL;
}

cJSON *store_secret_names(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < AUTO_MAX_SECRETS; i++) {
        if (s_secrets[i].value) cJSON_AddItemToArray(arr, cJSON_CreateString(s_secrets[i].name));
    }
    return arr;
}

int store_secret_values(const char **vals, int max)
{
    int n = 0;
    for (int i = 0; i < AUTO_MAX_SECRETS && n < max; i++) {
        if (s_secrets[i].value) vals[n++] = s_secrets[i].value;
    }
    return n;
}

// ---- time zone -------------------------------------------------------------------------------

static char s_tz[64] = "UTC0";
static bool s_tz_loaded;

const char *store_tz(void)
{
    if (!s_tz_loaded) {
        s_tz_loaded = true;
        nvs_handle_t h;
        if (nvs_open(AUTO_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_tz);
            if (nvs_get_str(h, "tz", s_tz, &len) != ESP_OK) strlcpy(s_tz, "UTC0", sizeof(s_tz));
            nvs_close(h);
        }
    }
    return s_tz;
}

esp_err_t store_set_tz(const char *tz)
{
    size_t n = strlen(tz);
    if (n == 0 || n >= sizeof(s_tz)) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++) {
        if (!isprint((unsigned char)tz[i]) || tz[i] == ' ') return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t r = nvs_open(AUTO_NS, NVS_READWRITE, &h);
    if (r == ESP_OK) {
        r = nvs_set_str(h, "tz", tz);
        if (r == ESP_OK) r = nvs_commit(h);
        nvs_close(h);
    }
    if (r != ESP_OK) return r;
    strlcpy(s_tz, tz, sizeof(s_tz));
    s_tz_loaded = true;
    setenv("TZ", s_tz, 1);
    tzset();
    ESP_LOGI(TAG, "time zone: %s", s_tz);
    return ESP_OK;
}

// ---- script state -------------------------------------------------------------------------------

static cJSON *s_state[AUTO_MAX_SCRIPTS];
static bool s_state_dirty[AUTO_MAX_SCRIPTS];
static SemaphoreHandle_t s_state_mtx, s_flush_mtx;

static void state_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, AUTO_STATE_DIR "/%s.json", name);
}

static void state_mutexes(void)
{
    if (!s_state_mtx) {
        s_state_mtx = xSemaphoreCreateMutex();
        s_flush_mtx = xSemaphoreCreateMutex();
    }
}

void store_state_load(int idx)
{
    state_mutexes();
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    bool have = s_state[idx] != NULL;
    xSemaphoreGive(s_state_mtx);
    if (have) return;
    char path[64];
    state_path(path, sizeof(path), g_scripts[idx].name);
    size_t len;
    char *txt = read_file(path, 128 * 1024, &len);
    cJSON *j = txt ? cJSON_ParseWithLength(txt, len) : NULL;
    free(txt);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        j = cJSON_CreateObject();
    }
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    if (!s_state[idx]) {
        s_state[idx] = j;
        j = NULL;
    }
    xSemaphoreGive(s_state_mtx);
    cJSON_Delete(j);
}

cJSON *store_state_lock(int idx)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    return s_state[idx];
}

void store_state_unlock(int idx, bool dirty)
{
    if (dirty) s_state_dirty[idx] = true;
    xSemaphoreGive(s_state_mtx);
}

void store_state_forget(int idx, bool delete_file)
{
    char path[64];
    state_path(path, sizeof(path), g_scripts[idx].name);
    xSemaphoreTake(s_flush_mtx, portMAX_DELAY);
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    cJSON_Delete(s_state[idx]);
    s_state[idx] = NULL;
    s_state_dirty[idx] = false;
    xSemaphoreGive(s_state_mtx);
    if (delete_file) unlink(path);
    xSemaphoreGive(s_flush_mtx);
}

void store_state_flush(void)
{
    if (!s_state_mtx || xSemaphoreTake(s_flush_mtx, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        char *txt = NULL;
        char path[64];
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        if (s_state_dirty[i] && s_state[i]) {
            txt = cJSON_PrintUnformatted(s_state[i]);
            if (txt) s_state_dirty[i] = false;
            state_path(path, sizeof(path), g_scripts[i].name);
        }
        xSemaphoreGive(s_state_mtx);
        if (!txt) continue;
        if (write_file(path, txt, strlen(txt)) != ESP_OK) {
            xSemaphoreTake(s_state_mtx, portMAX_DELAY);
            s_state_dirty[i] = true;  // retried on the next round
            xSemaphoreGive(s_state_mtx);
        }
        cJSON_free(txt);
    }
    xSemaphoreGive(s_flush_mtx);
}

static void io_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        store_state_flush();
    }
}

void store_io_start(void)
{
    state_mutexes();
    xTaskCreate(io_task, "auto_io", 3072, NULL, 2, NULL);
}
