/*
 * ============================================================
 *  NexLink — ESP32 I/O Controller
 *  Multi-client | JSON Protocol | Auth | Telemetry | PWM
 *
 *  
 *  Company   : praveenkumar self-hosted project
 *  
 *  Version   : 2.0.0
 * ============================================================
 *
 *  JSON Protocol (newline-terminated):
 *  ─────────────────────────────────────────────────────────
 *  Request:
 *    { "cmd": "LED_ON",  "token": "NEXLINK2024", "pin": 2 }
 *    { "cmd": "PWM",     "token": "NEXLINK2024", "pin": 18, "duty": 75 }
 *    { "cmd": "STATUS",  "token": "NEXLINK2024" }
 *    { "cmd": "PING",    "token": "NEXLINK2024" }
 *    { "cmd": "TELEMETRY","token": "NEXLINK2024" }
 *
 *  Response:
 *    { "status": "ok",  "cmd": "LED_ON",  "pin": 2, "state": 1 }
 *    { "status": "err", "code": "AUTH_FAIL", "msg": "bad token" }
 *    { "status": "ok",  "cmd": "TELEMETRY",
 *      "uptime_s": 342, "free_heap": 210432,
 *      "clients": 2, "gpio": {"2":1,"4":0,"5":0} }
 * ─────────────────────────────────────────────────────────
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* ── Wi-Fi credentials ──────────────────────────────────── */
#define WIFI_SSID      "ssid"
#define WIFI_PASS      "pssword"
#define WIFI_MAX_RETRY    5

/* ── Server config ──────────────────────────────────────── */
#define TCP_PORT          3333
#define MAX_CLIENTS       4        /* FreeRTOS tasks spawned  */
#define RX_BUF_SIZE       256
#define TX_BUF_SIZE       512
#define AUTH_TOKEN        "NEXLINK2024"
#define HEARTBEAT_MS      10000    /* 10-sec PING timeout      */

/* ── GPIO pin table ─────────────────────────────────────── */
#define NUM_GPIO_PINS     3
static const gpio_num_t GPIO_PINS[NUM_GPIO_PINS] = {
    GPIO_NUM_2, GPIO_NUM_4, GPIO_NUM_5
};
static volatile bool gpio_states[NUM_GPIO_PINS] = {false};

/* ── PWM (LEDC) config ──────────────────────────────────── */
#define NUM_PWM_PINS      2
static const gpio_num_t PWM_PINS[NUM_PWM_PINS] = {
    GPIO_NUM_18, GPIO_NUM_19
};
static volatile uint32_t pwm_duties[NUM_PWM_PINS] = {0};
#define PWM_FREQ_HZ       5000
#define PWM_RESOLUTION    LEDC_TIMER_10_BIT   /* 0–1023 */

/* ── Tag & globals ──────────────────────────────────────── */
static const char *TAG = "NEXLINK";

static EventGroupHandle_t  wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int          wifi_retry    = 0;
static SemaphoreHandle_t gpio_mutex;       /* protect gpio_states  */
static volatile int active_clients = 0;
static SemaphoreHandle_t client_mutex;

/* ── Minimal JSON helpers (no cJSON dependency) ──────────── */
/*
 * json_get_str  – extracts value of "key":"VALUE"  into out (max len)
 * json_get_int  – extracts value of "key":NUMBER   into out
 * Returns true on success.
 */
static bool json_get_str(const char *json, const char *key,
                         char *out, size_t maxlen)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < maxlen - 1) out[i++] = *p++;
        out[i] = '\0';
        return true;
    }
    return false;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = (int)strtol(p, NULL, 10);
        return true;
    }
    return false;
}

/* ── GPIO helpers ───────────────────────────────────────── */
static int gpio_pin_index(int pin)
{
    for (int i = 0; i < NUM_GPIO_PINS; i++)
        if ((int)GPIO_PINS[i] == pin) return i;
    return -1;
}

static int pwm_pin_index(int pin)
{
    for (int i = 0; i < NUM_PWM_PINS; i++)
        if ((int)PWM_PINS[i] == pin) return i;
    return -1;
}

static void set_gpio(int idx, bool on)
{
    xSemaphoreTake(gpio_mutex, portMAX_DELAY);
    gpio_states[idx] = on;
    gpio_set_level(GPIO_PINS[idx], on ? 1 : 0);
    xSemaphoreGive(gpio_mutex);
}

static void set_pwm(int idx, uint32_t duty_pct)
{
    if (duty_pct > 100) duty_pct = 100;
    uint32_t raw = (duty_pct * 1023) / 100;
    pwm_duties[idx] = duty_pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE,
                  (ledc_channel_t)idx, raw);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,
                     (ledc_channel_t)idx);
}

/* ── Command dispatcher ─────────────────────────────────── */
/*
 * process_command
 *   json_req  – null-terminated JSON string from client
 *   resp      – output buffer for JSON response
 *   resp_len  – size of resp buffer
 */
static void process_command(const char *json_req,
                             char *resp, size_t resp_len)
{
    char token[64] = {0};
    char cmd[32]   = {0};

    /* ── Auth check ───────────────────────────────────────── */
    if (!json_get_str(json_req, "token", token, sizeof(token)) ||
        strcmp(token, AUTH_TOKEN) != 0)
    {
        snprintf(resp, resp_len,
            "{\"status\":\"err\",\"code\":\"AUTH_FAIL\","
            "\"msg\":\"invalid or missing token\"}\n");
        return;
    }

    if (!json_get_str(json_req, "cmd", cmd, sizeof(cmd))) {
        snprintf(resp, resp_len,
            "{\"status\":\"err\",\"code\":\"BAD_CMD\","
            "\"msg\":\"missing cmd field\"}\n");
        return;
    }

    /* ── PING ─────────────────────────────────────────────── */
    if (strcmp(cmd, "PING") == 0) {
        snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"PING\","
            "\"msg\":\"pong\",\"uptime_ms\":%" PRId64 "}\n",
            esp_timer_get_time() / 1000);
        return;
    }

    /* ── TELEMETRY ─────────────────────────────────────────── */
    if (strcmp(cmd, "TELEMETRY") == 0) {
        xSemaphoreTake(gpio_mutex, portMAX_DELAY);
        int used = snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"TELEMETRY\","
            "\"uptime_s\":%" PRId64 ","
            "\"free_heap\":%" PRIu32 ","
            "\"clients\":%d,"
            "\"gpio\":{",
            esp_timer_get_time() / 1000000,
            esp_get_free_heap_size(),
            active_clients);
        for (int i = 0; i < NUM_GPIO_PINS && used < (int)resp_len - 20; i++) {
            used += snprintf(resp + used, resp_len - used,
                "\"%d\":%d%s",
                (int)GPIO_PINS[i],
                gpio_states[i] ? 1 : 0,
                i < NUM_GPIO_PINS - 1 ? "," : "");
        }
        snprintf(resp + used, resp_len - used,
            "},\"pwm\":{\"%" PRIu32 "\":%" PRIu32 ","
                       "\"%" PRIu32 "\":%" PRIu32 "}}\n",
            (uint32_t)PWM_PINS[0], pwm_duties[0],
            (uint32_t)PWM_PINS[1], pwm_duties[1]);
        xSemaphoreGive(gpio_mutex);
        return;
    }

    /* ── STATUS ───────────────────────────────────────────── */
    if (strcmp(cmd, "STATUS") == 0) {
        xSemaphoreTake(gpio_mutex, portMAX_DELAY);
        int used = snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"STATUS\",\"gpio\":{");
        for (int i = 0; i < NUM_GPIO_PINS && used < (int)resp_len - 20; i++) {
            used += snprintf(resp + used, resp_len - used,
                "\"%d\":%d%s",
                (int)GPIO_PINS[i],
                gpio_states[i] ? 1 : 0,
                i < NUM_GPIO_PINS - 1 ? "," : "");
        }
        snprintf(resp + used, resp_len - used, "}}\n");
        xSemaphoreGive(gpio_mutex);
        return;
    }

    /* ── LED_ON / LED_OFF ─────────────────────────────────── */
    if (strcmp(cmd, "LED_ON") == 0 || strcmp(cmd, "LED_OFF") == 0) {
        int pin = (int)GPIO_PINS[0];   /* default pin 2        */
        json_get_int(json_req, "pin", &pin);
        int idx = gpio_pin_index(pin);
        if (idx < 0) {
            snprintf(resp, resp_len,
                "{\"status\":\"err\",\"code\":\"BAD_PIN\","
                "\"pin\":%d}\n", pin);
            return;
        }
        bool on = (strcmp(cmd, "LED_ON") == 0);
        set_gpio(idx, on);
        snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"%s\","
            "\"pin\":%d,\"state\":%d}\n",
            cmd, pin, on ? 1 : 0);
        return;
    }

    /* ── ALL_OFF ──────────────────────────────────────────── */
    if (strcmp(cmd, "ALL_OFF") == 0) {
        for (int i = 0; i < NUM_GPIO_PINS; i++) set_gpio(i, false);
        for (int i = 0; i < NUM_PWM_PINS;  i++) set_pwm(i, 0);
        snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"ALL_OFF\"}\n");
        return;
    }

    /* ── PWM ──────────────────────────────────────────────── */
    if (strcmp(cmd, "PWM") == 0) {
        int pin  = (int)PWM_PINS[0];
        int duty = 50;
        json_get_int(json_req, "pin",  &pin);
        json_get_int(json_req, "duty", &duty);
        int idx = pwm_pin_index(pin);
        if (idx < 0) {
            snprintf(resp, resp_len,
                "{\"status\":\"err\",\"code\":\"BAD_PIN\","
                "\"pin\":%d}\n", pin);
            return;
        }
        set_pwm(idx, (uint32_t)duty);
        snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"PWM\","
            "\"pin\":%d,\"duty_pct\":%d}\n", pin, duty);
        return;
    }

    /* ── REBOOT ───────────────────────────────────────────── */
    if (strcmp(cmd, "REBOOT") == 0) {
        snprintf(resp, resp_len,
            "{\"status\":\"ok\",\"cmd\":\"REBOOT\","
            "\"msg\":\"rebooting in 1s\"}\n");
        /* Caller sends this, then we restart */
        return;
    }

    /* ── Unknown ──────────────────────────────────────────── */
    snprintf(resp, resp_len,
        "{\"status\":\"err\",\"code\":\"UNKNOWN_CMD\","
        "\"cmd\":\"%s\"}\n", cmd);
}

/* ── Per-client task ─────────────────────────────────────── */
typedef struct {
    int  sock;
    char remote_ip[32];
} client_ctx_t;

static void client_task(void *pvArg)
{
    client_ctx_t *ctx = (client_ctx_t *)pvArg;
    int sock = ctx->sock;

    xSemaphoreTake(client_mutex, portMAX_DELAY);
    active_clients++;
    xSemaphoreGive(client_mutex);

    ESP_LOGI(TAG, "[%s] connected  (active: %d)",
             ctx->remote_ip, active_clients);

    /* Send banner */
    const char *banner =
        "{\"status\":\"ok\",\"msg\":\"NexLink v2.0.0 — ESP32 I/O Controller\","
        "\"auth\":\"required\",\"token_field\":\"token\"}\n";
    send(sock, banner, strlen(banner), 0);

    char   rx[RX_BUF_SIZE] = {0};
    char   tx[TX_BUF_SIZE] = {0};
    bool   do_reboot = false;
    TickType_t last_rx = xTaskGetTickCount();

    while (1) {
        /* Heartbeat: close idle connections */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_rx) * portTICK_PERIOD_MS > HEARTBEAT_MS) {
            ESP_LOGW(TAG, "[%s] heartbeat timeout", ctx->remote_ip);
            break;
        }

        /* Non-blocking recv with short poll */
        int n = recv(sock, rx, sizeof(rx) - 1, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            break;   /* real error */
        }
        if (n == 0) break;   /* client closed */

        rx[n] = '\0';
        last_rx = xTaskGetTickCount();

        /* Strip trailing CR/LF */
        for (int i = n - 1; i >= 0; i--) {
            if (rx[i] == '\r' || rx[i] == '\n') rx[i] = '\0';
            else break;
        }

        ESP_LOGI(TAG, "[%s] RX: %s", ctx->remote_ip, rx);

        /* Check for REBOOT before dispatching (need to send first) */
        char cmd_check[16] = {0};
        json_get_str(rx, "cmd", cmd_check, sizeof(cmd_check));
        if (strcmp(cmd_check, "REBOOT") == 0) do_reboot = true;

        memset(tx, 0, sizeof(tx));
        process_command(rx, tx, sizeof(tx));

        send(sock, tx, strlen(tx), 0);
        ESP_LOGI(TAG, "[%s] TX: %s", ctx->remote_ip, tx);

        if (do_reboot) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }

    close(sock);

    xSemaphoreTake(client_mutex, portMAX_DELAY);
    active_clients--;
    xSemaphoreGive(client_mutex);

    ESP_LOGI(TAG, "[%s] disconnected (active: %d)",
             ctx->remote_ip, active_clients);
    free(ctx);
    vTaskDelete(NULL);
}

/* ── TCP accept loop ─────────────────────────────────────── */
static void tcp_server_task(void *pvParameters)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_ERROR_CHECK(listen_sock < 0 ? ESP_FAIL : ESP_OK);

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dest = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family      = AF_INET,
        .sin_port        = htons(TCP_PORT),
    };
    bind(listen_sock, (struct sockaddr *)&dest, sizeof(dest));
    listen(listen_sock, MAX_CLIENTS);

    ESP_LOGI(TAG, "TCP server ready on port %d  (max %d clients)",
             TCP_PORT, MAX_CLIENTS);

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int sock = accept(listen_sock, (struct sockaddr *)&src, &slen);
        if (sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        xSemaphoreTake(client_mutex, portMAX_DELAY);
        int cur = active_clients;
        xSemaphoreGive(client_mutex);

        if (cur >= MAX_CLIENTS) {
            const char *full =
                "{\"status\":\"err\",\"code\":\"SERVER_FULL\"}\n";
            send(sock, full, strlen(full), 0);
            close(sock);
            ESP_LOGW(TAG, "Server full — client rejected");
            continue;
        }

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        ctx->sock = sock;
        inet_ntoa_r(src.sin_addr, ctx->remote_ip,
                    sizeof(ctx->remote_ip));

        /* Spawn a dedicated FreeRTOS task per client */
        char task_name[24];
        snprintf(task_name, sizeof(task_name), "cli_%.19s", ctx->remote_ip);
        xTaskCreate(client_task, task_name, 4096, ctx, 5, NULL);
    }
}

/* ── Wi-Fi ───────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT &&
               id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", wifi_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "═══════════════════════════════════");
        ESP_LOGI(TAG, "  IP : " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "  Connect Python client to:");
        ESP_LOGI(TAG, "    " IPSTR ":%d", IP2STR(&e->ip_info.ip), TCP_PORT);
        ESP_LOGI(TAG, "  Auth token : %s", AUTH_TOKEN);
        ESP_LOGI(TAG, "═══════════════════════════════════");
        wifi_retry = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi connection failed — restarting");
        esp_restart();
    }
}

/* ── PWM init ────────────────────────────────────────────── */
static void pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    for (int i = 0; i < NUM_PWM_PINS; i++) {
        ledc_channel_config_t ch = {
            .channel    = (ledc_channel_t)i,
            .duty       = 0,
            .gpio_num   = (int)PWM_PINS[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER_0,
        };
        ledc_channel_config(&ch);
    }
}

/* ── app_main ────────────────────────────────────────────── */
void app_main(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║   NexLink — ESP32 I/O Controller      ║\n");
    printf("║   Version 2.0.0          ║\n");
    printf("║   Multi-client | JSON | Auth | PWM   ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* GPIO init */
    for (int i = 0; i < NUM_GPIO_PINS; i++) {
        gpio_reset_pin(GPIO_PINS[i]);
        gpio_set_direction(GPIO_PINS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_PINS[i], 0);
    }

    /* PWM init */
    pwm_init();

    /* Mutexes */
    gpio_mutex   = xSemaphoreCreateMutex();
    client_mutex = xSemaphoreCreateMutex();

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Wi-Fi */
    wifi_init();

    /* TCP Server (runs forever) */
    xTaskCreate(tcp_server_task, "tcp_server", 6144, NULL, 5, NULL);
}