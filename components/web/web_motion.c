// REST API for Wi-Fi motion sensing (Settings → Motion). Both handlers need a session.
#include "cJSON.h"
#include "motion.h"
#include "web_internal.h"

static esp_err_t h_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    motion_status_t s;
    motion_get_status(&s);
    static const char *const LEARN[] = {"", "still", "moving"};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "sensing", s.sensing);
    cJSON_AddBoolToObject(o, "moving", s.moving);
    cJSON_AddStringToObject(o, "learning", s.learning < 3 ? LEARN[s.learning] : "");
    if (s.score >= 0) cJSON_AddNumberToObject(o, "score", s.score);
    else cJSON_AddNullToObject(o, "score");
    cJSON_AddNumberToObject(o, "threshold", s.threshold);
    cJSON_AddNumberToObject(o, "fps", s.fps);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = web_send_json(req, "200 OK", txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t h_learn(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char buf[64];
    if (web_read_body(req, buf, sizeof(buf)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "body too large");
    }
    cJSON *j = cJSON_Parse(buf);
    const cJSON *m = cJSON_GetObjectItem(j, "moving");
    bool ok = cJSON_IsBool(m);
    if (ok) motion_learn(cJSON_IsTrue(m));
    cJSON_Delete(j);
    if (!ok) return web_send_error(req, "400 Bad Request", "expected {\"moving\": true|false}");
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t motion_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/motion", .method = HTTP_GET, .handler = h_get},
        {.uri = "/api/motion/learn", .method = HTTP_POST, .handler = h_learn},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
