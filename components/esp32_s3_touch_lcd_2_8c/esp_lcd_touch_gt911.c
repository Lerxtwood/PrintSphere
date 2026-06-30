#include "esp_lcd_touch.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_lcd_2_8c.h"

static const char *TAG = "gt911";

#define ESP_LCD_TOUCH_GT911_READ_XY_REG (0x814E)
#define ESP_LCD_TOUCH_GT911_CONFIG_REG (0x8047)
#define ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG (0x8140)
#define BSP_TOUCH_RESET_MASK IO_EXPANDER_PIN_NUM_1

static esp_err_t gt911_read_data(esp_lcd_touch_handle_t tp);
static bool gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                         uint16_t *strength, uint8_t *point_num,
                         uint8_t max_point_num);
static esp_err_t gt911_del(esp_lcd_touch_handle_t tp);

static esp_err_t gt911_i2c_read(esp_lcd_touch_handle_t tp, uint16_t reg,
                                uint8_t *data, uint8_t len) {
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t gt911_i2c_write(esp_lcd_touch_handle_t tp, uint16_t reg,
                                 uint8_t data, uint8_t len) {
    return esp_lcd_panel_io_tx_param(tp->io, reg, (uint8_t[]){data}, len);
}

static esp_err_t gt911_reset(esp_lcd_touch_handle_t tp) {
    esp_io_expander_handle_t io = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(io != NULL, ESP_FAIL, TAG, "io expander unavailable");

    gpio_set_direction(BSP_LCD_TOUCH_INT, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_LCD_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, BSP_TOUCH_RESET_MASK, 0), TAG,
                        "touch reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, BSP_TOUCH_RESET_MASK, 1), TAG,
                        "touch reset high failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(BSP_LCD_TOUCH_INT, 1);
    gpio_set_direction(BSP_LCD_TOUCH_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

static void gt911_log_config(esp_lcd_touch_handle_t tp) {
    uint8_t buf[4] = {};
    if (gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG, &buf[0], 3) == ESP_OK &&
        gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_CONFIG_REG, &buf[3], 1) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 id=%02x%02x%02x cfg=%u", buf[0], buf[1], buf[2], buf[3]);
    }
}

esp_err_t esp_lcd_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_touch_config_t *config,
                                      esp_lcd_touch_handle_t *out_touch) {
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t touch = NULL;

    ESP_RETURN_ON_FALSE(io != NULL && config != NULL && out_touch != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "invalid args");

    touch = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE(touch != NULL, ESP_ERR_NO_MEM, err, TAG, "alloc failed");

    touch->io = io;
    touch->read_data = gt911_read_data;
    touch->get_xy = gt911_get_xy;
    touch->del = gt911_del;
    touch->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&touch->config, config, sizeof(*config));

    if (touch->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << touch->config.int_gpio_num,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err, TAG, "int gpio config failed");
    }

    ESP_GOTO_ON_ERROR(gt911_reset(touch), err, TAG, "reset failed");
    gt911_log_config(touch);

    *out_touch = touch;
    return ESP_OK;

err:
    if (touch != NULL) {
        gt911_del(touch);
    }
    return ret;
}

static esp_err_t gt911_read_data(esp_lcd_touch_handle_t tp) {
    uint8_t buf[1 + (8 * CONFIG_ESP_LCD_TOUCH_MAX_POINTS)] = {};
    uint8_t touch_cnt = 0;

    ESP_RETURN_ON_ERROR(gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_READ_XY_REG, buf, 1), TAG,
                        "status read failed");

    if ((buf[0] & 0x80U) == 0) {
        (void)gt911_i2c_write(tp, ESP_LCD_TOUCH_GT911_READ_XY_REG, 0, 1);
        return ESP_OK;
    }

    touch_cnt = buf[0] & 0x0FU;
    if (touch_cnt == 0 || touch_cnt > 5) {
        (void)gt911_i2c_write(tp, ESP_LCD_TOUCH_GT911_READ_XY_REG, 0, 1);
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        gt911_i2c_read(tp, ESP_LCD_TOUCH_GT911_READ_XY_REG + 1, &buf[1], touch_cnt * 8), TAG,
        "point read failed");
    ESP_RETURN_ON_ERROR(gt911_i2c_write(tp, ESP_LCD_TOUCH_GT911_READ_XY_REG, 0, 1), TAG,
                        "clear failed");

    taskENTER_CRITICAL(&tp->data.lock);
    tp->data.points =
        touch_cnt > CONFIG_ESP_LCD_TOUCH_MAX_POINTS ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : touch_cnt;
    for (size_t i = 0; i < tp->data.points; ++i) {
        tp->data.coords[i].x = ((uint16_t)buf[(i * 8) + 3] << 8) | buf[(i * 8) + 2];
        tp->data.coords[i].y = ((uint16_t)buf[(i * 8) + 5] << 8) | buf[(i * 8) + 4];
        tp->data.coords[i].strength = ((uint16_t)buf[(i * 8) + 7] << 8) | buf[(i * 8) + 6];
    }
    taskEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool gt911_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                         uint16_t *strength, uint8_t *point_num,
                         uint8_t max_point_num) {
    taskENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (size_t i = 0; i < *point_num; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength != NULL) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    taskEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t gt911_del(esp_lcd_touch_handle_t tp) {
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    free(tp);
    return ESP_OK;
}
