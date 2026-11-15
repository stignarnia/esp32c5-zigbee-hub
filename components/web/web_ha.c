// REST API for the Home Assistant integration (Settings → Home Assistant). Session needed.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esphome_api.h"
#include "web.h"
#include "tailscale.h"
#include "web_internal.h"

static esp_err_t reply(httpd_req_t *req)
{
    ha_status_t s;
    ha_get_status(&s);
    ts_status_t t;
    ts_get_status(&t);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s.enabled);
    cJSON_AddBoolToObject(o, "running", s.running);
    cJSON_AddNumberToObject(o, "port", HA_API_PORT);
    cJSON_AddNumberToObject(o, "clients", s.clients);
    cJSON_AddNumberToObject(o, "entities", s.entities);
    cJSON_AddNumberToObject(o, "devices", s.devices);
    cJSON_AddStringToObject(o, "key", s.key);
    cJSON_AddStringToObject(o, "client", s.client);
    cJSON_AddNumberToObject(o, "connects", s.connects);
    cJSON_AddNumberToObject(o, "rejected", s.rejected);
    cJSON_AddStringToObject(o, "hostname", web_hostname());
    cJSON_AddBoolToObject(o, "lan_open", web_lan_open());
    cJSON_AddStringToObject(o, "tailnet_name", t.name);
    memset(s.key, 0, sizeof(s.key));
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = web_send_json(req, "200 OK", txt);
    memset(txt, 0, strlen(txt));
    cJSON_free(txt);
    return err;
}

static esp_err_t h_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    return reply(req);
}

// {"on": true|false}
static esp_err_t h_set(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char buf[64];
    if (web_read_body(req, buf, sizeof(buf)) != ESP_OK) return web_send_error(req, "400 Bad Request", "body too large");
    cJSON *j = cJSON_Parse(buf);
    const cJSON *on = cJSON_GetObjectItem(j, "on");
    bool ok = cJSON_IsBool(on), want = cJSON_IsTrue(on);
    cJSON_Delete(j);
    if (!ok) return web_send_error(req, "400 Bad Request", "expected {\"on\": true|false}");
    esp_err_t err = ha_set_enabled(want);
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return reply(req);
}

static esp_err_t h_key(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    esp_err_t err = ha_new_key();
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return reply(req);
}

esp_err_t ha_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/ha", .method = HTTP_GET, .handler = h_get},
        {.uri = "/api/ha", .method = HTTP_POST, .handler = h_set},
        {.uri = "/api/ha/key", .method = HTTP_POST, .handler = h_key},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
