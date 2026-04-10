#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "tusb.h"

// FreeRTOS & BTstack includes
#include "FreeRTOS.h"
#include "task.h"
#include "btstack.h"
#include "pico/cyw43_arch.h"

// Our custom BLE Server
#include "ble_server.h"

#include "pico/async_context_freertos.h"
static async_context_freertos_t async_context_instance;

// --- Z-Stack MT Commands ---
static uint8_t ZB_PING[]    = {0xFE, 0x00, 0x21, 0x01, 0x20};
static uint8_t ZB_STARTUP[] = {0xFE, 0x02, 0x25, 0x40, 0x00, 0x00, 0x67};
static uint8_t ZB_PERMIT[]  = {0xFE, 0x04, 0x25, 0x36, 0x00, 0x00, 0xFF, 0x00, 0xE8};
static uint8_t ZB_LQI_REQ[] = {0xFE, 0x03, 0x25, 0x31, 0x00, 0x00, 0x00, 0x17};
static uint8_t ZB_NV_STARTUP_CLEAR[] = {0xFE, 0x05, 0x21, 0x09, 0x03, 0x00, 0x00, 0x01, 0x03, 0x2C};
static uint8_t ZB_RESET[]   = {0xFE, 0x01, 0x21, 0x00, 0x00, 0x20};

static uint8_t ZB_AF_REG[] = {
    0xFE, 0x0D, 0x24, 0x00, 
    0x01,             // EndPoint
    0x04, 0x01,       // AppProfID: 0x0104 (HA)
    0x05, 0x00,       // AppDeviceId
    0x00,             // AppDevVer
    0x00,             // Latency
    0x02,             // NumInClusters (2)
    0x00, 0x00,       // Cluster 1: 0x0000
    0x03, 0x00,       // Cluster 2: 0x0003
    0x00              // NumOutClusters
};

bool ping_ok = false, started = false, registered = false, paired = false, nv_cleared = false;

#define MAX_KNOWN 16
uint16_t known_devices[MAX_KNOWN];
uint8_t known_count = 0;

bool is_known(uint16_t nwk) {
    for (int i = 0; i < known_count; i++) {
        if (known_devices[i] == nwk) return true;
    }
    return false;
}

uint8_t calculate_fcs(uint8_t *msg, uint8_t len) {
    uint8_t fcs = 0;
    for (int i = 1; i < len - 1; i++) fcs ^= msg[i];
    return fcs;
}

// Updated to yield to FreeRTOS scheduler
void delay_usb(uint32_t ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < ms) {
        tuh_task(); // Keep USB stack alive
        vTaskDelay(pdMS_TO_TICKS(1)); // Yield to BTstack and other tasks
    }
}

void request_ieee_and_name(uint16_t nwk_addr) {
    // 1. Send IEEE Addr Request
    uint8_t ieee_msg[] = {0xFE, 0x05, 0x25, 0x01, (uint8_t)(nwk_addr & 0xFF), (uint8_t)(nwk_addr >> 8), 0x00, 0x00, 0x00, 0x00};
    ieee_msg[9] = calculate_fcs(ieee_msg, 10);
    tuh_cdc_write(0, ieee_msg, 10);
    tuh_cdc_write_flush(0);
    
    delay_usb(100);

    // 2. Send Name Request
    uint8_t msg[] = {
        0xFE, 0x0F, 0x24, 0x01, 
        (uint8_t)(nwk_addr & 0xFF), (uint8_t)(nwk_addr >> 8),
        0x01, 0x01,       
        0x00, 0x00,       
        0x01, 0x30, 0x07, 0x03,
        0x00, 0x12, 0x00, 0x05, 0x00, 0x00
    };
    msg[19] = calculate_fcs(msg, 20);
    tuh_cdc_write(0, msg, 20);
    tuh_cdc_write_flush(0);
    
    delay_usb(100); 
}

void parse_zstack_frame(uint8_t cmd0, uint8_t cmd1, uint8_t *payload, uint8_t len) {
    // Handle LQI / Neighbor Responses
    if (cmd0 == 0x45 && cmd1 == 0xB1) {
        uint8_t count = payload[3];
        printf("\n--- Found %d Neighbors ---\n", count);
        
        for (int n = 0; n < count; n++) {
            int off = 4 + (n * 22);
            uint16_t nwk = payload[off + 16] | (payload[off + 17] << 8);
            if (nwk != 0x0000 && nwk != 0xFFFF) {
                if (!is_known(nwk)) { 
                    printf("Attempting Query on 0x%04X...\n", nwk);
                    request_ieee_and_name(nwk);
                    if (known_count < MAX_KNOWN) {
                        known_devices[known_count++] = nwk;
                    }
                }
            }
        }
    }

    // Handle Incoming ZCL Data
    if (cmd0 == 0x44 && cmd1 == 0x81) {
        uint16_t cluster = payload[2] | (payload[3] << 8);
        
        // Extract Source Network Address (AF_INCOMING_MSG format)
        uint16_t src_nwk = payload[4] | (payload[5] << 8);

        // Basic Cluster (0x0000) for Device Name
        if (cluster == 0x0000) { 
            for (int i = 0; i < len - 2; i++) {
                if (payload[i] == 0x42) { 
                    uint8_t sLen = payload[i+1];
                    if (sLen > 0 && sLen < 32 && (i + 2 + sLen <= len)) {
                        bool is_text = true;
                        for (int c = 0; c < sLen; c++) {
                            if (payload[i+2+c] < 32 || payload[i+2+c] > 126) {
                                is_text = false;
                                break;
                            }
                        }
                        if (is_text) {
                            printf(">> DEVICE NAME (0x%04X): %.*s\n", src_nwk, sLen, &payload[i+2]);
                            break; 
                        }
                    }
                }
            }
        }

        // Handle On/Off Cluster (0x0006) for Door/Window State
        if (cluster == 0x0006) {
            if (payload[19] == 0x0A) { 
                uint16_t attr_id = payload[20] | (payload[21] << 8);
                
                if (attr_id == 0x0000) {
                    uint8_t data_type = payload[22];
                    uint8_t state = payload[23]; // 0 = Closed, 1 = Open
                    
                    if (state == 0x01) {
                        printf("\n[SENSOR 0x%04X] Door/Window OPENED!\n", src_nwk);
                    } else if (state == 0x00) {
                        printf("\n[SENSOR 0x%04X] Door/Window CLOSED!\n", src_nwk);
                    }

                    // Push the new state to the BLE queue
                    uint8_t ble_payload[1] = {state};
                    ble_server_enqueue_event(src_nwk, cluster, ble_payload, 1);
                }
            }
        }
    }
}

void tuh_cdc_rx_cb(uint8_t itf) {
    uint8_t buf[512];
    uint32_t count = tuh_cdc_read(itf, buf, sizeof(buf));
    for (uint32_t i = 0; i < count; i++) {
        if (buf[i] == 0xFE && (i + 4 < count)) {
            uint8_t pLen = buf[i+1];
            if (buf[i+2] == 0x61 && buf[i+3] == 0x01) ping_ok = true;
            parse_zstack_frame(buf[i+2], buf[i+3], &buf[i+4], pLen);
            i += (pLen + 4);
        }
    }
}

// --- FreeRTOS Tasks & Callbacks ---

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    // Route HCI events directly to our custom BLE server
    ble_server_handle_hci_event(packet_type, channel, packet, size);
}

void zigbee_usb_task(void *params) {
    // Initialize CYW43 (Spawns the background worker task)
    if (cyw43_arch_init() != PICO_OK) {
        printf("FATAL: Failed to initialize CYW43 Architecture\n");
        vTaskDelete(NULL);
    }

    // Initialize Standard BTstack Components
    // CRITICAL: We must lock the async context before calling any BTstack APIs!
    cyw43_thread_enter();
    
    l2cap_init();
    sm_init();
    ble_server_init(packet_handler);
    hci_power_control(HCI_POWER_ON);
    ble_server_start_advertising();
    
    cyw43_thread_exit(); // Release the lock so the background worker can run
    // ------------------------------------------------------------------------

    // Initialize TinyUSB
    tusb_init();
    
    uint32_t last_step_ms = 0;
    
    while (1) {
        tuh_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        if (tuh_cdc_mounted(0) && (now - last_step_ms > 2000)) {
            if (!ping_ok) {
                printf("[INIT] Pinging Dongle...\n");
                tuh_cdc_write(0, ZB_PING, sizeof(ZB_PING));
            } else if (!started) {
                printf("[INIT] Starting Zigbee Stack...\n");
                tuh_cdc_write(0, ZB_STARTUP, sizeof(ZB_STARTUP));
                started = true;
            } else if (!registered) {
                printf("[INIT] Registering Application Endpoint...\n");
                ZB_AF_REG[17] = calculate_fcs(ZB_AF_REG, 18);
                tuh_cdc_write(0, ZB_AF_REG, 18);
                registered = true;
            } else if (!paired) {
                printf("[INIT] Opening Network...\n");
                tuh_cdc_write(0, ZB_PERMIT, sizeof(ZB_PERMIT));
                paired = true;
            } else {
                tuh_cdc_write(0, ZB_LQI_REQ, sizeof(ZB_LQI_REQ));
            }
            tuh_cdc_write_flush(0);
            last_step_ms = now;
        }

        // Yield to the scheduler
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// --- FreeRTOS Error Hooks ---

void vApplicationMallocFailedHook(void) {
    // Called if a call to pvPortMalloc() fails because there is insufficient
    // free memory available in the FreeRTOS heap.
    printf("FATAL: FreeRTOS Malloc Failed!\n");
    panic("Out of memory");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    // Called if a task blows past its allocated stack space.
    printf("FATAL: FreeRTOS Stack Overflow in task: %s\n", pcTaskName);
    panic("Stack overflow");
}

int main() {
    stdio_init_all();
    sleep_ms(8000); // Initial boot delay

    // Initialize the FreeRTOS Async Context
    async_context_freertos_config_t config = async_context_freertos_default_config();
    if (!async_context_freertos_init(&async_context_instance, &config)) {
        printf("FATAL: Failed to initialize async context\n");
        return -1;
    }
    
    // Set this as the default context
    cyw43_arch_set_async_context(&async_context_instance.core);

    // Create the unified Zigbee/USB/BTstack task
    xTaskCreate(
        zigbee_usb_task, 
        "ZigbeeUSB", 
        2048, 
        NULL, 
        tskIDLE_PRIORITY + 2, 
        NULL
    );

    // Start the Scheduler (CYW43 and BTstack init will happen inside the task)
    vTaskStartScheduler();

    // Cleanup (Should never reach here)
    async_context_deinit(&async_context_instance.core);
    return 0;
}