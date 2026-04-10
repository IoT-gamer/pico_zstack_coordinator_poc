/*
 * ble_server.c
 *
 * BLE server implementation for the Pico Zigbee-to-BLE Bridge.
 * Implements an asynchronous FreeRTOS Queue to safely pass data from the 
 * TinyUSB Host task into the single-threaded BTstack environment.
 */

#include "ble_server.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "zigbee_bridge.h" 

// --- Globals ---
static hci_con_handle_t server_con_handle = HCI_CON_HANDLE_INVALID;

// FreeRTOS Queue Handle
static QueueHandle_t zigbee_to_ble_queue = NULL;

// BTstack Timer to periodically poll the FreeRTOS queue
static btstack_timer_source_t queue_poll_timer;

// State management for notifications
static zigbee_event_t active_event;
static bool waiting_for_can_send_now = false;

// --- Advertising Data ---
static uint8_t adv_data[] = {
    // Flags: General Discoverable
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Name: "Pico-Zigbee"
    0x0C, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 
    'P', 'i', 'c', 'o', '-', 'Z', 'i', 'g', 'b', 'e', 'e',
    // 16-bit Service UUIDs (Custom Service 0xFF00)
    0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS, 0x00, 0xFF
};
static const uint8_t adv_data_len = sizeof(adv_data);

// --- Forward Declarations ---
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size);
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static void queue_poll_timer_handler(btstack_timer_source_t *ts);

// --- Public API ---

void ble_server_init(btstack_packet_handler_t att_packet_handler) {
    // 1. Create the FreeRTOS Queue (e.g., hold up to 10 Zigbee events)
    zigbee_to_ble_queue = xQueueCreate(10, sizeof(zigbee_event_t));
    if (zigbee_to_ble_queue == NULL) {
        printf("FATAL: Failed to create zigbee_to_ble_queue\n");
    }

    // 2. Initialize ATT server with the profile data (from zigbee_bridge.h)
    att_server_init(profile_data, att_read_callback, att_write_callback);
    
    // 3. Register the main packet handler for ATT events
    att_server_register_packet_handler(att_packet_handler);

    // 4. Setup the BTstack polling timer (polls the FreeRTOS queue every 50ms)
    btstack_run_loop_set_timer_handler(&queue_poll_timer, queue_poll_timer_handler);
    btstack_run_loop_set_timer(&queue_poll_timer, 50);
    btstack_run_loop_add_timer(&queue_poll_timer);
}

void ble_server_start_advertising(void) {
    printf("Starting BLE advertising...\n");
    uint16_t adv_int = 800; // ~500ms
    uint8_t adv_type = 0;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    
    gap_advertisements_set_params(adv_int, adv_int, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, (uint8_t*) adv_data);
    gap_advertisements_enable(1);
}

void ble_server_stop_advertising(void) {
    gap_advertisements_enable(0);
}

bool ble_server_enqueue_event(uint16_t nwk_addr, uint16_t cluster_id, uint8_t *payload, uint8_t len) {
    if (zigbee_to_ble_queue == NULL) return false;
    if (len > MAX_ZIGBEE_PAYLOAD_LEN) len = MAX_ZIGBEE_PAYLOAD_LEN; // Truncate to avoid overflow

    zigbee_event_t event;
    event.nwk_addr = nwk_addr;
    event.cluster_id = cluster_id;
    event.payload_len = len;
    memcpy(event.payload, payload, len);

    // Try to push to queue without blocking (we don't want to freeze the USB task)
    if (xQueueSend(zigbee_to_ble_queue, &event, 0) != pdPASS) {
        printf("WARNING: BLE Queue full, dropping Zigbee event from 0x%04X\n", nwk_addr);
        return false;
    }
    return true;
}

// --- Internal Callbacks & Handlers ---

/**
 * @brief Periodic timer that runs inside the BTstack thread.
 * Checks the FreeRTOS queue and asks for radio permission if data is waiting.
 */
static void queue_poll_timer_handler(btstack_timer_source_t *ts) {
    // If we have a connected client and aren't already waiting for radio permission
    if (server_con_handle != HCI_CON_HANDLE_INVALID && !waiting_for_can_send_now) {
        
        // Peek/Pop from the FreeRTOS Queue
        if (xQueueReceive(zigbee_to_ble_queue, &active_event, 0) == pdPASS) {
            waiting_for_can_send_now = true;
            // Ask BTstack for permission to notify the client
            att_server_request_can_send_now_event(server_con_handle);
        }
    }

    // Re-arm the timer
    btstack_run_loop_set_timer(ts, 50);
    btstack_run_loop_add_timer(ts);
}

void ble_server_handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                server_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                printf("BLE Client Connected.\n");
                ble_server_stop_advertising();
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (hci_event_disconnection_complete_get_connection_handle(packet) == server_con_handle) {
                server_con_handle = HCI_CON_HANDLE_INVALID;
                waiting_for_can_send_now = false; // Reset state
                printf("BLE Client Disconnected.\n");
                ble_server_start_advertising();
            }
            break;

        case ATT_EVENT_CAN_SEND_NOW:
            waiting_for_can_send_now = false;

            // Pack the BLE Payload: [2 bytes NWK] [2 bytes Cluster] [N bytes Data]
            uint8_t notify_buffer[20];
            uint8_t total_len = 4 + active_event.payload_len;
            
            notify_buffer[0] = active_event.nwk_addr & 0xFF;
            notify_buffer[1] = (active_event.nwk_addr >> 8) & 0xFF;
            notify_buffer[2] = active_event.cluster_id & 0xFF;
            notify_buffer[3] = (active_event.cluster_id >> 8) & 0xFF;
            memcpy(&notify_buffer[4], active_event.payload, active_event.payload_len);

            att_server_notify(server_con_handle, 
                              ATT_CHARACTERISTIC_0000FF01_0000_1000_8000_00805F9B34FB_01_VALUE_HANDLE, 
                              notify_buffer, 
                              total_len);
            
            printf("BLE: Notified Device 0x%04X state change.\n", active_event.nwk_addr);
            break;
    }
}

static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size) {
    // Implement standard reads here if needed (e.g., reading bridge status)
    return 0; 
}

static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    
    // Handle incoming writes from the App (e.g., "Permit Join" command)
    if (att_handle == ATT_CHARACTERISTIC_0000FF02_0000_1000_8000_00805F9B34FB_01_VALUE_HANDLE) {
        if (buffer_size < 1) return 0;

        uint8_t cmd = buffer[0];
        printf("BLE Write Cmd: 0x%02X\n", cmd);

        switch(cmd) {
            case 0x01: // Open Network
                printf("Instructing Zigbee task to Open Network...\n");
                // TODO: Need to implement a reverse queue here to safely 
                // tell the TinyUSB task to send ZB_PERMIT to the dongle.
                break;
            default:
                printf("Unknown Command\n");
        }
    }
    return 0;
}