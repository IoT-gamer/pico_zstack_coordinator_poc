#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include "btstack.h"
#include <stdbool.h>
#include <stdint.h>

// FreeRTOS includes
#include "FreeRTOS.h"
#include "queue.h"

// Define a reasonable max payload size for Zigbee ZCL attributes (e.g., On/Off state)
#define MAX_ZIGBEE_PAYLOAD_LEN 16

// Struct to hold Zigbee events safely in the FreeRTOS queue
typedef struct {
    uint16_t nwk_addr;
    uint16_t cluster_id;
    uint8_t payload[MAX_ZIGBEE_PAYLOAD_LEN];
    uint8_t payload_len;
} zigbee_event_t;

/**
 * @brief Initialize the ATT server, GATT profile, and FreeRTOS queues.
 * @param att_packet_handler The main HCI event handler for the app.
 */
void ble_server_init(btstack_packet_handler_t att_packet_handler);

/**
 * @brief Start advertising the Pico as a peripheral.
 */
void ble_server_start_advertising(void);

/**
 * @brief Stop advertising.
 */
void ble_server_stop_advertising(void);

/**
 * @brief Called by the TinyUSB/Zigbee Task to safely push new sensor data to BLE.
 * @return true if successfully queued, false if the queue is full.
 */
bool ble_server_enqueue_event(uint16_t nwk_addr, uint16_t cluster_id, uint8_t *payload, uint8_t len);

/**
 * @brief Handle HCI events related to the server role (connection, disconnection, permissions).
 */
void ble_server_handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

#endif // BLE_SERVER_H