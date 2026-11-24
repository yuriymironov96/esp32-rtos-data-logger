#include "network.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_BIT BIT0
#define MQTT_BIT BIT1
static EventGroupHandle_t flags;
static QueueHandle_t events, outgoing;
static mqtt_client_t *client;
static char client_id[32];
static struct mqtt_connect_client_info_t client_info;
// MQTT state belongs exclusively to the lwIP TCP/IP thread.
static bool connecting, dns_pending, subscribed, subscribe_pending;
static int64_t next_connect;
static int incoming_field = -1;
static char payload[32];
static unsigned payload_used;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(flags, WIFI_BIT | MQTT_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW("wifi", "Reconnect: %s", esp_err_to_name(err));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(flags, WIFI_BIT);
        ESP_LOGI("wifi", "Connected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(flags, WIFI_BIT | MQTT_BIT);
    }
}

static void incoming_publish(void *arg, const char *topic, u32_t length)
{
    incoming_field = -1;
    payload_used = 0;
    const char prefix[] = MQTT_SUB_BASE "/";
    if (!length || length >= sizeof(payload) || strncmp(topic, prefix, sizeof(prefix) - 1)) {
        return;
    }
    const char *suffix = topic + sizeof(prefix) - 1;
    const char *fields[] = {"temp", "hum", "pres"};
    for (unsigned i = 0; i < 3; i++) {
        if (!strcmp(suffix, fields[i])) {
            incoming_field = (int)i;
        }
    }
}

static void incoming_data(void *arg, const u8_t *data, u16_t length, u8_t mqtt_flags)
{
    if (incoming_field < 0) {
        return;
    }
    if (payload_used + length >= sizeof(payload)) {
        incoming_field = -1;
        return;
    }
    memcpy(payload + payload_used, data, length);
    payload_used += length;
    if (!(mqtt_flags & MQTT_DATA_FLAG_LAST)) {
        return;
    }
    payload[payload_used] = '\0';
    char *end;
    float value = strtof(payload, &end);
    if (end == payload) {
        return;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    static const float minimum[] = {-40, 0, 200};
    static const float maximum[] = {85, 100, 900};
    if (*end || !isfinite(value) || value < minimum[incoming_field] ||
        value > maximum[incoming_field]) {
        return;
    }
    app_event_t event = {.type = EVENT_REMOTE, .remote = {.field = incoming_field, .value = value}};
    xQueueSend(events, &event, 0);
}

static void subscription_result(void *arg, err_t err)
{
    subscribe_pending = false;
    subscribed = err == ERR_OK;
    if (!subscribed) {
        ESP_LOGW("mqtt", "Subscription failed: %d", err);
    }
}

static void connection_result(mqtt_client_t *mqtt, void *arg, mqtt_connection_status_t result)
{
    connecting = false;
    subscribed = subscribe_pending = false;
    if (result == MQTT_CONNECT_ACCEPTED) {
        xEventGroupSetBits(flags, MQTT_BIT);
        ESP_LOGI("mqtt", "Connected to %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    } else {
        xEventGroupClearBits(flags, MQTT_BIT);
        next_connect = esp_timer_get_time() + 5000000;
        ESP_LOGW("mqtt", "Disconnected: %d", result);
    }
}

static void resolved(const char *name, const ip_addr_t *address, void *arg)
{
    dns_pending = false;
    if (!address || !network_wifi_connected()) {
        return;
    }
    err_t err = mqtt_client_connect(client, address, MQTT_BROKER_PORT, connection_result, NULL,
                                    &client_info);
    connecting = err == ERR_OK;
    if (err != ERR_OK) {
        ESP_LOGW("mqtt", "Connect failed: %d", err);
    }
}

static void process(void *arg)
{
    if (!client) {
        client = mqtt_client_new();
        configASSERT(client);
        mqtt_set_inpub_callback(client, incoming_publish, incoming_data, NULL);
    }
    if (!network_wifi_connected()) {
        mqtt_disconnect(client);
        connecting = subscribed = subscribe_pending = false;
        xEventGroupClearBits(flags, MQTT_BIT);
        return;
    }
    if (!mqtt_client_is_connected(client)) {
        int64_t now = esp_timer_get_time();
        if (connecting || dns_pending || now < next_connect) {
            return;
        }
        next_connect = now + 5000000;
        ip_addr_t address;
        err_t err = dns_gethostbyname(MQTT_BROKER_HOST, &address, resolved, NULL);
        if (err == ERR_OK) {
            resolved(NULL, &address, NULL);
        } else if (err == ERR_INPROGRESS) {
            dns_pending = true;
        } else {
            ESP_LOGW("mqtt", "DNS failed: %d", err);
        }
        return;
    }
    if (!subscribed && !subscribe_pending) {
        err_t err = mqtt_subscribe(client, MQTT_SUB_BASE "/+", 0, subscription_result, NULL);
        subscribe_pending = err == ERR_OK;
    }
    sample_t sample;
    if (xQueueReceive(outgoing, &sample, 0) != pdTRUE) {
        return;
    }
    const char *topics[] = {MQTT_PUB_BASE "/temp", MQTT_PUB_BASE "/hum", MQTT_PUB_BASE "/pres"};
    double values[] = {sample.record.temperature / 100.0, sample.record.humidity / 100.0,
                       sample.record.pressure};
    for (unsigned i = 0; i < 3; i++) {
        char text[24];
        int length = snprintf(text, sizeof(text), "%.2f", values[i]);
        err_t err = mqtt_publish(client, topics[i], text, length, 0, 0, NULL, NULL);
        if (err != ERR_OK) {
            ESP_LOGW("mqtt", "Publish dropped (%s): %d", topics[i], err);
        }
    }
}

void network_init(QueueHandle_t event_queue)
{
    events = event_queue;
    flags = xEventGroupCreate();
    outgoing = xQueueCreate(1, sizeof(sample_t));
    configASSERT(flags && outgoing);
    if (!strlen(WIFI_SSID)) {
        ESP_LOGW("wifi", "No SSID configured; local logging remains available");
        return;
    }
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(client_id, sizeof(client_id), "logger-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    client_info = (struct mqtt_connect_client_info_t){.client_id = client_id, .keep_alive = 30};
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *interface = esp_netif_create_default_wifi_sta();
    configASSERT(interface);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    wifi_config_t config = {0};
    _Static_assert(sizeof(WIFI_SSID) - 1 <= sizeof(config.sta.ssid), "SSID is too long");
    _Static_assert(sizeof(WIFI_PASSWORD) - 1 <= sizeof(config.sta.password),
                   "Password is too long");
    memcpy(config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void network_tick(void)
{
    if (!strlen(WIFI_SSID)) {
        return;
    }
    err_t err = tcpip_try_callback(process, NULL);
    if (err != ERR_OK) {
        ESP_LOGW("mqtt", "TCP/IP queue busy: %d", err);
    }
}

void network_publish(const sample_t *sample)
{
    if (sample->sensor_ok) {
        xQueueOverwrite(outgoing, sample);
    }
}

bool network_wifi_connected(void)
{
    return (xEventGroupGetBits(flags) & WIFI_BIT) != 0;
}
bool network_mqtt_connected(void)
{
    return (xEventGroupGetBits(flags) & MQTT_BIT) != 0;
}
