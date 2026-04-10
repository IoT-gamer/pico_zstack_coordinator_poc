# Pico Z-Stack Coordinator & BLE Bridge PoC

A C-based proof-of-concept demonstrating how to use a **Raspberry Pi Pico W / Pico 2 W** as a USB Host to control a **Texas Instruments Z-Stack Zigbee Coordinator** (specifically the **Sonoff Zigbee 3.0 USB Dongle Plus-P**), and bridge those Zigbee events to a smartphone via **Bluetooth Low Energy (BLE)**.

This project uses the Pico C/C++ SDK, TinyUSB, FreeRTOS, and BTstack. It sends raw Monitor and Test (MT) API commands directly to the CC2652P chip over a USB CDC connection and exposes device state changes through a custom BLE GATT server.

## 🚀 Features

* **Zigbee-to-BLE Bridge:** Automatically forwards Zigbee cluster state changes (such as a Door/Window sensor opening or closing) to a connected BLE client in real-time via GATT notifications.

* **FreeRTOS Architecture:** Built on FreeRTOS using a cooperative task-driven design, utilizing the Pico SDK's `async_context` to safely multiplex TinyUSB host polling and the BTstack wireless background worker.

* **USB Host CDC:** Utilizes TinyUSB in host mode to enumerate and communicate with the Sonoff Dongle.

* **Network Management:** Formats a Zigbee network, registers application endpoints, and opens the network for pairing (Permit Join).

* **Device Discovery & Active Interrogation:** Parses Link Quality Indicator (LQI) neighbor tables to discover newly paired devices, automatically requesting the IEEE address and Device Name (Basic Cluster `0x0000`).

* **ZCL Parsing:** Includes logic to parse Zigbee Cluster Library (ZCL) incoming messages, such as extracting plain-text names and reading the On/Off Cluster (0x0006).

* **Spam Prevention:** Maintains a `known_devices` list to prevent flooding the network with duplicate queries.

## 🛠 Hardware Requirements

1. **Raspberry Pi Pico W or Pico 2 W:** (*Note: A standard Pico will not work, as the CYW43 wireless chip is strictly required for the BLE server*).
2. **USB OTG Cable:** Micro-USB Male to USB-A Female adapter.
3. **Zigbee Coordinator:** Sonoff Zigbee 3.0 USB Dongle Plus (Model "P" / CC2652P).
4. **USB to TTL Serial Adapter:** For debugging and monitoring serial output from the Pico (optional but recommended).
5. **5V regulated power supply:** To power the Pico and the USB Dongle.
6. **Test Device:** A standard Zigbee end device (e.g., Xiaomi Aqara Door/Window sensor `lumi.sensor_magnet.aq2`).


## 🔌 Wiring
<img src="docs/images/wire.jpg" alt="drawing" height="300"/>

1. Connect the Sonoff Zigbee Dongle to the USB OTG adapter, and then connect it to the Raspberry Pi Pico micro-USB port.
2. If using a USB to TTL Serial Adapter, connect it to the Pico's UART pins for serial debugging.
    * Tx -> GPIO0 (UART0 TX)
    * Rx -> GPIO1 (UART0 RX)
    * GND -> GND
    * Power -> unused (power the Pico separately)
3. Power the Pico using a **regulated** 5V supply.
    * 5V -> VBUS
    * GND -> GND

## 📱 Testing the BLE Bridge (nRF Connect)
Once the Pico is running and your Zigbee sensor is paired:

1. **Download nRF Connect for Mobile** (iOS/Android).
2. Open the app and scan for Bluetooth devices. Look for **"Pico-Zigbee"**.
3. **Tap Connect.**
4. Scroll down to the **Custom Service** (`0000FF00-0000-1000-8000-00805F9B34FB`).
5. Tap the multiple downward arrows icon on the **Device Event Stream Characteristic** (`FF01`) to **Enable Notifications**.
6. Trigger your Zigbee sensor (e.g., open the door magnet). You will instantly see the hex payload stream into the app representing the network address, cluster ID, and state!

## 💻 Building and Flashing the Project

1. Use VSCode with the official [Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) for easier building and flashing.

2. `Ctlr+Shift+P` -> `CMake: Configure`

3. Click `Compile` in bottom bar.

4. Remove the regulated 5V power supply from VBUS

5. Remove the USB OTG adapter from the Pico, and connect the pico to your computer via USB. This will allow you to flash the firmware onto the Pico via USB mass storage.

6. Put your Pico 2 W into BOOTSEL mode (hold the BOOTSEL button while plugging it in).

7. Click `Run` in bottom bar.

8. Remove the USB from your computer and reconnect the USB OTG adapter to the Pico after flashing.

9. Attach the regulated 5V power supply to VBUS and GND. 

⚠️ Do not power the Pico through both USB and the regulated 5V supply at the same time, as this can cause damage. Always ensure that only one power source is connected at a time.

## 📜 License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgements
- [Z-Stack Monitor and Test API](https://dev.ti.com/tirex/explore/node?isTheia=false&node=A__ACh9le-N7XEoxabGqQcHgQ__com.ti.SIMPLELINK_CC13XX_CC26XX_SDK__BSEc4rl__LATEST)
- [TinyUSB](https://github.com/hathach/tinyusb)
- [Pico examples usb host](https://github.com/raspberrypi/pico-examples/tree/master/usb/host)
- [Pico examples FreeRTOS](https://github.com/raspberrypi/pico-examples/tree/master?tab=readme-ov-file#freertos)
- [Pico examples BTstack](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/bt/standalone)
- [BTstack](https://github.com/bluekitchen/btstack)
- [FreeRTOS](https://www.freertos.org/)