// GPIO/buzzer probe (Phase 1b, CLAUDE.md). Commands on the console UART (115200), one
// per line; every command ends with all pins back to plain inputs (no pulls):
//   r P                    idle level of pin P, read with the ADC
//   m P M C                drive P high (and M low, -1 = buzzer to GND) at drive strength
//                          C (0..3) for ~300 ms; ADC on P while driven; prints mV
//   b P M C ON OFF N       beep: N times ON ms on / OFF ms off at drive strength C
//   p A+ A- B+ B- C mask:ms ...   two buzzers, step pattern (mask 1 = A, 2 = B, 3 = both)
// Only pins that CLAUDE.md lists as usable are accepted, and on-times are capped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_ON_MS     2000
#define MAX_TOTAL_MS  10000

// Header pins that are not strapping (2,7,25,27,28), USB (13,14), UART0 (11,12), 32 kHz
// crystal (0,1) or flash.
static const int ALLOWED[] = {3, 4, 5, 6, 8, 9, 10, 15, 23, 24, 26};

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static bool allowed(int pin)
{
    for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
        if (ALLOWED[i] == pin) return true;
    }
    return false;
}

static void safe(int pin)
{
    if (pin < 0) return;
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

// Prepares the ADC channel of `pin` (must be ADC-capable). drive() then gives the pad back
// to the GPIO function (gpio_config), so it can be driven while the ADC samples it.
static bool adc_prepare(int pin, adc_channel_t *ch)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(pin, &unit, ch) != ESP_OK || unit != ADC_UNIT_1) return false;
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(s_adc, *ch, &cc) != ESP_OK) return false;
    return true;
}

// Averages n readings; also returns min/max mV.
static int adc_mv(adc_channel_t ch, int n, int *mn, int *mx)
{
    long sum = 0;
    *mn = 1 << 30;
    *mx = -1;
    for (int i = 0; i < n; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc, ch, &raw);
        if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        else mv = raw * 3300 / 4095;
        sum += mv;
        if (mv < *mn) *mn = mv;
        if (mv > *mx) *mx = mv;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return (int)(sum / n);
}

// gpio_config also selects the GPIO function on the pad (after the ADC took it).
static void out(int pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT_OUTPUT};
    gpio_config(&io);
}

static void drive(int plus, int minus, int cap, bool on)
{
    if (minus >= 0) {
        out(minus);
        gpio_set_drive_capability(minus, GPIO_DRIVE_CAP_3);
        gpio_set_level(minus, 0);
    }
    out(plus);
    gpio_set_drive_capability(plus, (gpio_drive_cap_t)cap);
    gpio_set_level(plus, on ? 1 : 0);
}

static void cmd_read(int pin)
{
    adc_channel_t ch;
    if (!adc_prepare(pin, &ch)) { printf("ERR pin %d has no ADC1 channel\n", pin); return; }
    safe(pin);
    int mn, mx, mv = adc_mv(ch, 20, &mn, &mx);
    printf("R pin=%d idle_mV=%d (%d..%d)\n", pin, mv, mn, mx);
}

static void cmd_measure(int plus, int minus, int cap)
{
    adc_channel_t ch;
    if (!adc_prepare(plus, &ch)) { printf("ERR pin %d has no ADC1 channel\n", plus); return; }
    drive(plus, minus, cap, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    int hmn, hmx, high = adc_mv(ch, 40, &hmn, &hmx);   // ~200 ms while driven high
    gpio_set_level(plus, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    int lmn, lmx, low = adc_mv(ch, 10, &lmn, &lmx);
    safe(plus);
    safe(minus);
    printf("M plus=%d minus=%d cap=%d high_mV=%d (%d..%d) low_mV=%d\n",
           plus, minus, cap, high, hmn, hmx, low);
}

static void cmd_beep(int plus, int minus, int cap, int on, int off, int n)
{
    if (on < 1 || on > MAX_ON_MS || off < 0 || off > MAX_ON_MS || n < 1 || n > 10 ||
        (on + off) * n > MAX_TOTAL_MS) {
        printf("ERR beep limits: on 1..%d ms, off 0..%d ms, n 1..10, total <= %d ms\n",
               MAX_ON_MS, MAX_ON_MS, MAX_TOTAL_MS);
        return;
    }
    for (int i = 0; i < n; i++) {
        drive(plus, minus, cap, true);
        vTaskDelay(pdMS_TO_TICKS(on));
        gpio_set_level(plus, 0);
        vTaskDelay(pdMS_TO_TICKS(off));
    }
    safe(plus);
    safe(minus);
    printf("B plus=%d minus=%d cap=%d on=%d off=%d n=%d done\n", plus, minus, cap, on, off, n);
}

// Two-buzzer pattern: "p A+ A- B+ B- C mask:ms mask:ms ..." — mask bit 0 = buzzer A on,
// bit 1 = buzzer B on (3 = both). Up to 64 steps; each step 1..2000 ms; total <= 10 s.
// Pins are checked like the other commands; A- / B- may be -1 (leg on GND).
#define PAT_MAX_STEPS 64
static void cmd_pattern(char *args)
{
    int pin[4], cap, n = 0, used = 0;
    if (sscanf(args, " %d %d %d %d %d%n", &pin[0], &pin[1], &pin[2], &pin[3], &cap, &used) != 5) {
        printf("ERR usage: p A+ A- B+ B- C mask:ms ...\n");
        return;
    }
    if (!allowed(pin[0]) || !allowed(pin[2]) || pin[0] == pin[2] || cap < 0 || cap > 3 ||
        (pin[1] != -1 && !allowed(pin[1])) || (pin[3] != -1 && !allowed(pin[3]))) {
        printf("ERR pin not allowed or bad drive strength\n");
        return;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (pin[i] >= 0 && pin[i] == pin[j]) { printf("ERR pins overlap\n"); return; }
        }
    }
    uint8_t mask[PAT_MAX_STEPS];
    uint16_t ms[PAT_MAX_STEPS];
    int total = 0;
    char *p = args + used;
    for (;;) {
        int m, t, k = 0;
        if (sscanf(p, " %d:%d%n", &m, &t, &k) != 2) break;
        if (n >= PAT_MAX_STEPS || m < 0 || m > 3 || t < 1 || t > MAX_ON_MS) {
            printf("ERR step %d: mask 0..3, ms 1..%d, at most %d steps\n", n, MAX_ON_MS,
                   PAT_MAX_STEPS);
            return;
        }
        mask[n] = (uint8_t)m;
        ms[n++] = (uint16_t)t;
        total += t;
        p += k;
    }
    if (n == 0 || total > MAX_TOTAL_MS) {
        printf("ERR need 1..%d steps, total <= %d ms\n", PAT_MAX_STEPS, MAX_TOTAL_MS);
        return;
    }
    for (int b = 0; b < 2; b++) {
        int plus = pin[b * 2], minus = pin[b * 2 + 1];
        if (minus >= 0) {
            out(minus);
            gpio_set_drive_capability(minus, GPIO_DRIVE_CAP_3);
            gpio_set_level(minus, 0);
        }
        out(plus);
        gpio_set_drive_capability(plus, (gpio_drive_cap_t)cap);
        gpio_set_level(plus, 0);
    }
    for (int i = 0; i < n; i++) {
        gpio_set_level(pin[0], mask[i] & 1);
        gpio_set_level(pin[2], (mask[i] >> 1) & 1);
        vTaskDelay(pdMS_TO_TICKS(ms[i]));
    }
    for (int i = 0; i < 4; i++) safe(pin[i]);
    printf("P steps=%d total_ms=%d done\n", n, total);
}

void app_main(void)
{
    // Why the last start happened (a restart while driving a buzzer is the thing to catch).
    printf("PROBE reset_reason=%d\n", (int)esp_reset_reason());
    // Stay in this slot until switched back with otatool (no rollback mid-test).
    esp_ota_mark_app_valid_cancel_rollback();

    for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) safe(ALLOWED[i]);

    adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&uc, &s_adc));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                          .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
#endif

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    setvbuf(stdin, NULL, _IONBF, 0);

    printf("PROBE ready (calibration %s). Commands: r P | m P M C | b P M C ON OFF N\n",
           s_cali ? "curve fitting" : "none, rough mV");
    static char line[480];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (line[0] == 'p') { cmd_pattern(line + 1); continue; }
        int a[6] = {0}, n = 0;
        char c = 0;
        n = sscanf(line, " %c %d %d %d %d %d %d", &c, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
        if (n < 2) continue;
        int plus = a[0], minus = (n >= 3) ? a[1] : -1, cap = (n >= 4) ? a[2] : 3;
        if (!allowed(plus) || (minus != -1 && (!allowed(minus) || minus == plus)) ||
            cap < 0 || cap > 3) {
            printf("ERR pin not allowed or bad drive strength\n");
            continue;
        }
        if (c == 'r') cmd_read(plus);
        else if (c == 'm') cmd_measure(plus, minus, cap);
        else if (c == 'b' && n == 7) cmd_beep(plus, minus, cap, a[3], a[4], a[5]);
        else printf("ERR unknown command\n");
    }
}
