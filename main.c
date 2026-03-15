#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "tusb.h"

// --- Z-Stack MT Commands ---
// Command: SYS_PING
static uint8_t ZB_PING[]    = {0xFE, 0x00, 0x21, 0x01, 0x20};
// Command: ZDO_STARTUP_FROM_APP
static uint8_t ZB_STARTUP[] = {0xFE, 0x02, 0x25, 0x40, 0x00, 0x00, 0x67}; 
// Command:ZDO_MGMT_PERMIT_JOIN_REQ
static uint8_t ZB_PERMIT[] = {0xFE, 0x04, 0x25, 0x36, 0x00, 0x00, 0xFF, 0x00, 0xE8};
// Command: ZDO_MGMT_LQI_REQ
static uint8_t ZB_LQI_REQ[] = {0xFE, 0x03, 0x25, 0x31, 0x00, 0x00, 0x00, 0x17}; 
// Command: SYS_OSAL_NV_WRITE_EXT
static uint8_t ZB_NV_STARTUP_CLEAR[] = {0xFE, 0x05, 0x21, 0x09, 0x03, 0x00, 0x00, 0x01, 0x03, 0x2C};
// Command: SYS_RESET_REQ
static uint8_t ZB_RESET[] = {0xFE, 0x01, 0x21, 0x00, 0x00, 0x20};

// AF_REGISTER: Expanded to include Basic (0x0000) and Identify (0x0003) clusters
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

void delay_usb(uint32_t ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < ms) {
        tuh_task(); // Keep USB stack alive during the delay!
    }
}

// ZDO_IEEE_ADDR_REQ (CMD0: 0x25, CMD1: 0x01)
// This forces the coordinator to find the device and wakes up sleepy devices
void request_ieee_and_name(uint16_t nwk_addr) {
    // 1. Send IEEE Addr Request
    uint8_t ieee_msg[] = {0xFE, 0x05, 0x25, 0x01, (uint8_t)(nwk_addr & 0xFF), (uint8_t)(nwk_addr >> 8), 0x00, 0x00, 0x00, 0x00};
    ieee_msg[9] = calculate_fcs(ieee_msg, 10);
    tuh_cdc_write(0, ieee_msg, 10);
    tuh_cdc_write_flush(0);
    
    delay_usb(100); // Give dongle time to process and USB to send

    // 2. Send Name Request (AF_DATA_REQUEST)
    uint8_t msg[] = {
        0xFE, 0x0F, 0x24, 0x01, 
        (uint8_t)(nwk_addr & 0xFF), (uint8_t)(nwk_addr >> 8),
        0x01, 0x01,       // DstEP, SrcEP
        0x00, 0x00,       // Cluster: 0x0000
        0x01, 0x30, 0x07, 0x03,
        0x00, 0x12, 0x00, 0x05, 0x00, 0x00
    };
    msg[19] = calculate_fcs(msg, 20);
    tuh_cdc_write(0, msg, 20);
    tuh_cdc_write_flush(0);
    
    delay_usb(100); // Give dongle time to process
}

void parse_zstack_frame(uint8_t cmd0, uint8_t cmd1, uint8_t *payload, uint8_t len) {
    
    // Handle LQI / Neighbor Responses
    if (cmd0 == 0x45 && cmd1 == 0xB1) {
        uint8_t count = payload[3];
        printf("\n--- Found %d Neighbors ---\n", count);
        
        for (int n = 0; n < count; n++) {
            int off = 4 + (n * 22);
            uint16_t nwk = payload[off + 16] | (payload[off + 17] << 8);
            
            // If the network address is valid
            if (nwk != 0x0000 && nwk != 0xFFFF) {
                // Only query the device if we haven't seen it before
                if (!is_known(nwk)) { 
                    printf("Attempting Query on 0x%04X...\n", nwk);
                    request_ieee_and_name(nwk);
                    
                    // Add to our known list so we don't query it again
                    if (known_count < MAX_KNOWN) {
                        known_devices[known_count++] = nwk;
                    }
                }
            }
        }
    }

    // Handle Incoming ZCL Data (like the Device Name)
    if (cmd0 == 0x44 && cmd1 == 0x81) {
        // Extract the Cluster ID (LSB at payload[2], MSB at payload[3])
        uint16_t cluster = payload[2] | (payload[3] << 8);
        
        // We only expect standard device names in the Basic Cluster (0x0000)
        if (cluster == 0x0000) { 
            for (int i = 0; i < len - 2; i++) {
                
                // Look for the ZCL "Character String" data type tag
                if (payload[i] == 0x42) { 
                    uint8_t sLen = payload[i+1];
                    
                    // Prevent buffer overflow and verify the length is sane
                    if (sLen > 0 && sLen < 32 && (i + 2 + sLen <= len)) {
                        
                        // Verify all characters are standard printable ASCII
                        bool is_text = true;
                        for (int c = 0; c < sLen; c++) {
                            if (payload[i+2+c] < 32 || payload[i+2+c] > 126) {
                                is_text = false; 
                                break;
                            }
                        }
                        
                        // If it passed the text check, print it and stop scanning this packet
                        if (is_text) {
                            printf(">> DEVICE NAME (0x%02X%02X): %.*s\n", payload[1], payload[0], sLen, &payload[i+2]);
                            break; 
                        }
                    }
                }
            }
        }

        // Handle On/Off Cluster (0x0006) for Door/Window State
        if (cluster == 0x0006) {
            // The actual ZCL payload starts at payload[17] in an AF_INCOMING_MSG
            // A standard "Report Attributes" command has the ZCL Command ID 0x0A
            if (payload[19] == 0x0A) { 
                uint16_t attr_id = payload[20] | (payload[21] << 8);
                
                // Attribute 0x0000 is the On/Off state
                if (attr_id == 0x0000) {
                    uint8_t data_type = payload[22];
                    uint8_t state = payload[23]; // 0 = Closed, 1 = Open
                    
                    if (state == 0x01) {
                        printf("\n[SENSOR] Door/Window OPENED!\n");
                    } else if (state == 0x00) {
                        printf("\n[SENSOR] Door/Window CLOSED!\n");
                    }
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

int main() {
    stdio_init_all();
    sleep_ms(8000);

    tusb_init();
    uint32_t last_step_ms = 0;
    while (1) {
        tuh_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (tuh_cdc_mounted(0) && (now - last_step_ms > 2000)) {
            // if (!nv_cleared) { // temporary flag to run this once
            //     printf("[INIT] Clearing NV Memory...\n");
            //     tuh_cdc_write(0, ZB_NV_STARTUP_CLEAR, sizeof(ZB_NV_STARTUP_CLEAR));
            //     tuh_cdc_write_flush(0);
            //     delay_usb(200); 

            //     printf("[INIT] Resetting Dongle...\n");
            //     tuh_cdc_write(0, ZB_RESET, sizeof(ZB_RESET));
            //     tuh_cdc_write_flush(0);
                
            //     // Keep USB active while dongle reboots behind the UART bridge
            //     delay_usb(3000); 
            //     nv_cleared = true;
            // } else if (!ping_ok) {
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
    }
}