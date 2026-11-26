#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define CONTROL_GPIO GPIO_NUM_2
static const char *TAG = "OTG_CTRL";

static void trim_and_upper(char *str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == ' ')) {
        str[--len] = '\0';
    }
    for (size_t i = 0; i < len; i++) {
        str[i] = (char)toupper((unsigned char)str[i]);
    }
}

void app_main(void) {
    // Configure output GPIO
    gpio_reset_pin(CONTROL_GPIO);
    gpio_set_direction(CONTROL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONTROL_GPIO, 0);

    int current_state = 0;
    char cmd[64];

    // Disable stdout buffering so responses appear immediately on Android
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n=== ESP32-S3 N16R8 OTG Controller Ready ===\n");
    printf("Commands: ON (or 1), OFF (or 0), STATUS, HELP\n> ");

    while (1) {
        if (fgets(cmd, sizeof(cmd), stdin) != NULL) {
            trim_and_upper(cmd);

            if (strlen(cmd) == 0) {
                printf("> ");
                continue;
            }

            if (strcmp(cmd, "ON") == 0 || strcmp(cmd, "1") == 0) {
                current_state = 1;
                gpio_set_level(CONTROL_GPIO, current_state);
                printf("OK: GPIO %d is now ON\n> ", CONTROL_GPIO);
            } else if (strcmp(cmd, "OFF") == 0 || strcmp(cmd, "0") == 0) {
                current_state = 0;
                gpio_set_level(CONTROL_GPIO, current_state);
                printf("OK: GPIO %d is now OFF\n> ", CONTROL_GPIO);
            } else if (strcmp(cmd, "STATUS") == 0) {
                printf("STATUS: GPIO %d = %s\n> ", CONTROL_GPIO, current_state ? "ON" : "OFF");
            } else if (strcmp(cmd, "HELP") == 0) {
                printf("Available commands: ON, OFF, 1, 0, STATUS, HELP\n> ");
            } else {
                printf("UNKNOWN COMMAND: '%s' (Type HELP)\n> ", cmd);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
