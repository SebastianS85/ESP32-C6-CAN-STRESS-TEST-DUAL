#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = "CAN_STRESS_TEST";

// --- Pin Assignments ---
#define TX1_GPIO GPIO_NUM_13
#define RX1_GPIO GPIO_NUM_12
#define TX2_GPIO GPIO_NUM_11
#define RX2_GPIO GPIO_NUM_10

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} can_msg_t;

twai_node_handle_t node1_hdl = NULL;
twai_node_handle_t node2_hdl = NULL;
QueueHandle_t rx_queue1 = NULL;
QueueHandle_t rx_queue2 = NULL;
TaskHandle_t recovery_task1_hdl = NULL;
TaskHandle_t recovery_task2_hdl = NULL;

// --- ISR Callback ---
bool twai_rx_cb(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx) {
    uint8_t local_buf[8];
    twai_frame_t rx_frame = { .buffer = local_buf, .buffer_len = sizeof(local_buf) };

    if (twai_node_receive_from_isr(handle, &rx_frame) == ESP_OK) {
        can_msg_t msg;
        msg.id = rx_frame.header.id;
        msg.dlc = rx_frame.header.dlc;
        memcpy(msg.data, rx_frame.buffer, (msg.dlc > 8) ? 8 : msg.dlc);

        QueueHandle_t target_queue = (QueueHandle_t)user_ctx;
        BaseType_t xTaskWoken = pdFALSE;
        xQueueSendFromISR(target_queue, &msg, &xTaskWoken);
        return xTaskWoken == pdTRUE;
    }
    return false;
}

// --- Hardware Initialization ---
esp_err_t twai_system_init(void) {
    rx_queue1 = xQueueCreate(100, sizeof(can_msg_t));
    rx_queue2 = xQueueCreate(100, sizeof(can_msg_t));

    twai_onchip_node_config_t node_cfg = {
        .bit_timing = { .bitrate = 1000000 },
        .tx_queue_depth = 100,
    };

    node_cfg.io_cfg.tx = TX1_GPIO;
    node_cfg.io_cfg.rx = RX1_GPIO;
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &node1_hdl));

    node_cfg.io_cfg.tx = TX2_GPIO;
    node_cfg.io_cfg.rx = RX2_GPIO;
    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &node2_hdl));

    twai_event_callbacks_t cbs = { .on_rx_done = twai_rx_cb };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node1_hdl, &cbs, rx_queue1));
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node2_hdl, &cbs, rx_queue2));

    ESP_ERROR_CHECK(twai_node_enable(node1_hdl));
    ESP_ERROR_CHECK(twai_node_enable(node2_hdl));

    return ESP_OK;
}

// --- Recovery Task (one per node) ---
typedef struct {
    twai_node_handle_t hdl;
    const char *name;
} recovery_task_arg_t;

static recovery_task_arg_t recovery_arg1 = { .name = "Node 1" };
static recovery_task_arg_t recovery_arg2 = { .name = "Node 2" };

static void recovery_task(void *arg) {
    recovery_task_arg_t *ctx = (recovery_task_arg_t *)arg;
    while (1) {
        // Wait indefinitely for a notification from monitor_task
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "%s BUS-OFF detected, starting recovery...", ctx->name);
        esp_err_t err = twai_node_recover(ctx->hdl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s twai_node_recover failed: %s", ctx->name, esp_err_to_name(err));
            continue;
        }
        // Wait until node exits BUS-OFF (CAN spec: 128 * 11 recessive bits)
        twai_node_status_t status = {0};
        for (int i = 0; i < 50; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            twai_node_get_info(ctx->hdl, &status, NULL);
            if (status.state != TWAI_ERROR_BUS_OFF) {
                ESP_LOGI(TAG, "%s recovery complete.", ctx->name);
                break;
            }
        }
        if (status.state == TWAI_ERROR_BUS_OFF) {
            ESP_LOGE(TAG, "%s recovery timed out, still BUS-OFF.", ctx->name);
        }
    }
}

// --- Monitor Task ---
void monitor_task(void *arg) {
    can_msg_t msg;
    uint32_t count1 = 0, count2 = 0;
    uint64_t last_time = esp_timer_get_time();

    while (1) {
        while (xQueueReceive(rx_queue1, &msg, 0) == pdTRUE) count1++;
        while (xQueueReceive(rx_queue2, &msg, 0) == pdTRUE) count2++;

        uint64_t now = esp_timer_get_time();

        if ((now - last_time) >= 1000000) {
            twai_node_status_t s1 = {0};
            twai_node_status_t s2 = {0};
            twai_node_get_info(node1_hdl, &s1, NULL);
            twai_node_get_info(node2_hdl, &s2, NULL);

            ESP_LOGI(TAG, "N1 -> Rx: %lu msg/s | TX Err: %u | RX Err: %u",
                     count1, s1.tx_error_count, s1.rx_error_count);
            ESP_LOGI(TAG, "N2 -> Rx: %lu msg/s | TX Err: %u | RX Err: %u",
                     count2, s2.tx_error_count, s2.rx_error_count);

            // Bus recovery: notify dedicated recovery tasks in parallel
            if (s1.state == TWAI_ERROR_BUS_OFF) {
                xTaskNotifyGive(recovery_task1_hdl);
            }
            if (s2.state == TWAI_ERROR_BUS_OFF) {
                xTaskNotifyGive(recovery_task2_hdl);
            }

            count1 = 0; count2 = 0;
            last_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- Transmit Tasks ---
void tx_task_node1(void *arg) {
    uint8_t data[8] = {0x1A, 0x1B, 0x1C, 0x1D, 0x01, 0x02, 0x03, 0x04};
    twai_frame_t frame = { .header = {.id = 0x111, .dlc = 8}, .buffer = data, .buffer_len = 8 };

    while (1) {
        esp_err_t err = twai_node_transmit(node1_hdl, &frame, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void tx_task_node2(void *arg) {
    uint8_t data[8] = {0x2A, 0x2B, 0x2C, 0x2D, 0x05, 0x06, 0x07, 0x08};
    twai_frame_t frame = { .header = {.id = 0x222, .dlc = 8}, .buffer = data, .buffer_len = 8 };

    vTaskDelay(pdMS_TO_TICKS(5));

    while (1) {
        esp_err_t err = twai_node_transmit(node2_hdl, &frame, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// --- Main ---
void app_main(void) {
    if (twai_system_init() == ESP_OK) {
        ESP_LOGI(TAG, "Hardware initialized. Starting 1 Mbps Stress Test...");
        recovery_arg1.hdl = node1_hdl;
        recovery_arg2.hdl = node2_hdl;
        xTaskCreate(recovery_task, "REC1", 4096, &recovery_arg1, 8, &recovery_task1_hdl);
        xTaskCreate(recovery_task, "REC2", 4096, &recovery_arg2, 8, &recovery_task2_hdl);
        xTaskCreate(monitor_task,  "MON",  4096, NULL, 10, NULL);
        xTaskCreate(tx_task_node1, "TX1",  4096, NULL, 5, NULL);
        xTaskCreate(tx_task_node2, "TX2",  4096, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "TWAI hardware initialization failed!");
    }
}
