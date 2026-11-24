/*
 * ESP32-S3-WROOM-1 N16R8 firmware (ESP-IDF v5)
 *  - NVS, Wi-Fi STA (auto-reconnect), Bluedroid BLE advertising
 *  - ST7789 (esp_lcd) + LVGL 9 via esp_lvgl_port, draw buffers in Octal PSRAM
 *  - Sensor task (I2C SHT3x + status GPIO) updating the UI under lvgl_port_lock
 *  - HTTPS OTA via esp_https_ota (triggered by BOOT button long-press)
 *
 * N16R8: GPIO 33..37 are used by Octal PSRAM and MUST NOT be used.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "app";

/* ------------------------------------------------------------------------- */
/* Configuration (edit to match your hardware / network)                     */
/* ------------------------------------------------------------------------- */
#define WIFI_SSID            "YOUR_WIFI_SSID"
#define WIFI_PASS            "YOUR_WIFI_PASSWORD"
#define OTA_FIRMWARE_URL     "https://example.com/firmware/esp32s3_firmware.bin"
#define BLE_DEVICE_NAME      "ESP32S3-N16R8"

/* Display: ST7789 SPI 240x320 (example pins from the spec) */
#define LCD_HOST             SPI2_HOST
#define LCD_PIN_MOSI         GPIO_NUM_11
#define LCD_PIN_SCLK         GPIO_NUM_12
#define LCD_PIN_CS           GPIO_NUM_10
#define LCD_PIN_DC           GPIO_NUM_9
#define LCD_PIN_RST          GPIO_NUM_8
#define LCD_PIN_BL           GPIO_NUM_7
#define LCD_H_RES            240
#define LCD_V_RES            320
#define LCD_PIXEL_CLK_HZ     (40 * 1000 * 1000)
#define LCD_DRAW_BUF_LINES   40   /* lines per LVGL draw buffer */

/* Sensors / GPIO */
#define I2C_PIN_SDA          GPIO_NUM_4
#define I2C_PIN_SCL          GPIO_NUM_5
#define I2C_FREQ_HZ          100000
#define SENSOR_I2C_ADDR      0x44   /* SHT3x default address */
#define STATUS_LED_GPIO      GPIO_NUM_2
#define OTA_BUTTON_GPIO      GPIO_NUM_0   /* BOOT button, active low */
#define SENSOR_PERIOD_MS     2000
#define OTA_LONG_PRESS_MS    2000

/* Compile-time guard: keep away from the Octal PSRAM pins on N16R8 */
#define IS_RESERVED_PIN(p)   ((p) >= 33 && (p) <= 37)
_Static_assert(!IS_RESERVED_PIN(LCD_PIN_MOSI) && !IS_RESERVED_PIN(LCD_PIN_SCLK) &&
               !IS_RESERVED_PIN(LCD_PIN_CS)   && !IS_RESERVED_PIN(LCD_PIN_DC)   &&
               !IS_RESERVED_PIN(LCD_PIN_RST)  && !IS_RESERVED_PIN(LCD_PIN_BL)   &&
               !IS_RESERVED_PIN(I2C_PIN_SDA)  && !IS_RESERVED_PIN(I2C_PIN_SCL)  &&
               !IS_RESERVED_PIN(STATUS_LED_GPIO) && !IS_RESERVED_PIN(OTA_BUTTON_GPIO),
               "GPIO 33-37 are reserved for Octal PSRAM on ESP32-S3 N16R8");

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */
#define WIFI_CONNECTED_BIT   BIT0

static EventGroupHandle_t s_wifi_events;
static volatile bool s_wifi_connected;
static volatile bool s_ble_advertising;
static volatile bool s_ota_running;
static esp_ip4_addr_t s_ip;

static lv_display_t *s_disp;
static lv_obj_t *s_lbl_wifi, *s_lbl_ble, *s_lbl_temp, *s_lbl_hum, *s_lbl_led,
                *s_lbl_mem, *s_lbl_ver, *s_lbl_ota;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;

/* ------------------------------------------------------------------------- */
/* NVS                                                                       */
/* ------------------------------------------------------------------------- */
static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), erasing and retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ------------------------------------------------------------------------- */
/* Display + LVGL                                                            */
/* ------------------------------------------------------------------------- */
static void display_init(void)
{
    /* Backlight */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_PIN_BL, 0);

    const spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* LVGL port: draw buffers are allocated in Octal PSRAM (MALLOC_CAP_SPIRAM) */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,   /* heap_caps_malloc(..., MALLOC_CAP_SPIRAM) */
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    assert(s_disp != NULL);

    gpio_set_level(LCD_PIN_BL, 1);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

static void ui_create(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-S3 Status");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C2FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, LCD_H_RES - 16, LCD_V_RES - 52);
    lv_obj_align(col, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(col, lv_color_hex(0x1B2733), 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_row(col, 8, 0);

    s_lbl_wifi = make_row(col, "Wi-Fi: connecting...");
    s_lbl_ble  = make_row(col, "BLE: starting...");
    s_lbl_temp = make_row(col, "Temp: --.- C");
    s_lbl_hum  = make_row(col, "Humidity: --.- %");
    s_lbl_led  = make_row(col, "LED: OFF");
    s_lbl_mem  = make_row(col, "Heap/PSRAM: -");
    s_lbl_ota  = make_row(col, "OTA: idle (hold BOOT)");
    char ver[64];
    snprintf(ver, sizeof(ver), "FW: %s", app->version);
    s_lbl_ver  = make_row(col, ver);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------------- */
/* Wi-Fi station with auto-reconnect                                         */
/* ------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_ip = ev->ip_info.ip;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&s_ip));
    }
}

static void wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, WIFI_SSID, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, WIFI_PASS, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------------- */
/* Bluedroid BLE (advertising only)                                          */
/* ------------------------------------------------------------------------- */
static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CONN_ANY,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_ble_advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        ESP_LOGI(TAG, "BLE advertising %s", s_ble_advertising ? "started" : "failed");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_ble_advertising = false;
        break;
    default:
        break;
    }
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(BLE_DEVICE_NAME));

    esp_ble_adv_data_t adv = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
    };
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&adv));
}

/* ------------------------------------------------------------------------- */
/* Sensors                                                                   */
/* ------------------------------------------------------------------------- */
static void sensors_init(void)
{
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(STATUS_LED_GPIO, 0);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << OTA_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SENSOR_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev));
}

/* SHT3x single-shot, high repeatability, no clock stretching */
static esp_err_t sht3x_read(float *temp_c, float *hum_pct)
{
    const uint8_t cmd[2] = { 0x24, 0x00 };
    uint8_t rx[6];

    esp_err_t err = i2c_master_transmit(s_i2c_dev, cmd, sizeof(cmd), 100);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    err = i2c_master_receive(s_i2c_dev, rx, sizeof(rx), 100);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t t_raw = ((uint16_t)rx[0] << 8) | rx[1];
    uint16_t h_raw = ((uint16_t)rx[3] << 8) | rx[4];
    *temp_c  = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
    *hum_pct = 100.0f * ((float)h_raw / 65535.0f);
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    bool led = false;
    while (1) {
        float t = 0, h = 0;
        bool ok = (sht3x_read(&t, &h) == ESP_OK);

        led = !led;
        gpio_set_level(STATUS_LED_GPIO, led);

        char buf[64];
        lvgl_port_lock(0);

        if (s_wifi_connected) {
            snprintf(buf, sizeof(buf), "Wi-Fi: " IPSTR, IP2STR(&s_ip));
        } else {
            snprintf(buf, sizeof(buf), "Wi-Fi: connecting...");
        }
        lv_label_set_text(s_lbl_wifi, buf);

        lv_label_set_text(s_lbl_ble, s_ble_advertising ? "BLE: advertising" : "BLE: idle");

        if (ok) {
            snprintf(buf, sizeof(buf), "Temp: %.1f C", t);
            lv_label_set_text(s_lbl_temp, buf);
            snprintf(buf, sizeof(buf), "Humidity: %.1f %%", h);
            lv_label_set_text(s_lbl_hum, buf);
        } else {
            lv_label_set_text(s_lbl_temp, "Temp: n/a");
            lv_label_set_text(s_lbl_hum, "Humidity: n/a");
        }

        lv_label_set_text(s_lbl_led, led ? "LED: ON" : "LED: OFF");

        snprintf(buf, sizeof(buf), "Heap %uK  PSRAM %uK",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_label_set_text(s_lbl_mem, buf);

        lvgl_port_unlock();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* HTTPS OTA                                                                 */
/* ------------------------------------------------------------------------- */
static void ota_set_status(const char *msg)
{
    ESP_LOGI(TAG, "OTA: %s", msg);
    if (lvgl_port_lock(100)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "OTA: %s", msg);
        lv_label_set_text(s_lbl_ota, buf);
        lvgl_port_unlock();
    }
}

static esp_err_t ota_perform(const char *url)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    ESP_LOGI(TAG, "Current %s, remote %s", cur->version, new_desc.version);
    if (strcmp(cur->version, new_desc.version) == 0) {
        ota_set_status("already up to date");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    int last_pct = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(handle);
        int done = esp_https_ota_get_image_len_read(handle);
        int pct = total > 0 ? (done * 100) / total : 0;
        if (pct != last_pct && (pct % 5) == 0) {
            last_pct = pct;
            char m[32];
            snprintf(m, sizeof(m), "downloading %d%%", pct);
            ota_set_status(m);
        }
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }
    return esp_https_ota_finish(handle);
}

static void ota_task(void *arg)
{
    const char *url = (const char *)arg;

    ota_set_status("waiting for Wi-Fi");
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ota_set_status("starting");
    esp_err_t err = ota_perform(url);
    if (err == ESP_OK) {
        ota_set_status("success, rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else if (err != ESP_ERR_INVALID_VERSION) {
        char m[48];
        snprintf(m, sizeof(m), "failed (%s)", esp_err_to_name(err));
        ota_set_status(m);
    }
    s_ota_running = false;
    vTaskDelete(NULL);
}

static void ota_start(void)
{
    if (s_ota_running) {
        return;
    }
    s_ota_running = true;
    if (xTaskCreate(ota_task, "ota_task", 8192, (void *)OTA_FIRMWARE_URL, 5, NULL) != pdPASS) {
        s_ota_running = false;
        ESP_LOGE(TAG, "Failed to create OTA task");
    }
}

/* BOOT button long-press starts an OTA update */
static void button_task(void *arg)
{
    int held_ms = 0;
    while (1) {
        if (gpio_get_level(OTA_BUTTON_GPIO) == 0) {
            held_ms += 50;
            if (held_ms == OTA_LONG_PRESS_MS) {
                ota_start();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */
void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware %s (IDF %s)", app->version, app->idf_ver);

    nvs_init();
    ESP_LOGI(TAG, "PSRAM free: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    display_init();
    ui_create();

    sensors_init();
    wifi_init_sta();
    ble_init();

    /* Mark this image valid so a rollback-enabled bootloader keeps it */
    esp_ota_mark_app_valid_cancel_rollback();

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 4, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 3, NULL);
}
