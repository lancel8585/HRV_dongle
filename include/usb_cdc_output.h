#ifndef USB_CDC_OUTPUT_H
#define USB_CDC_OUTPUT_H

#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"

// Native USB CDC instance for data output (separate from debug UART)
extern USBCDC USBData;

// USB packet structure
#define USB_HEADER_BYTE 0xAA
#define USB_PACKET_TOTAL_SIZE                                                  \
  23 // Header(1) + DeviceIndex(1) + Status(1) + Code(1) + Data(18) +
     // Checksum(1)
#define USB_TX_BUFFER_SIZE 512 // Ring buffer size

// USB output packet format
typedef struct {
  uint8_t header;       // 0xAA fixed header
  uint8_t device_index; // Device index 0~3
  uint8_t status;       // Bit7: Sensor On/Off, Bit0-4: Packet Counter
  uint8_t code;         // Currently 0x80
  uint8_t data[18];     // Raw ECG data
  uint8_t checksum;     // XOR checksum of bytes 0-21
} __attribute__((packed)) usb_packet_t;

// Status packet for device connection events
typedef struct {
  uint8_t header;       // 0xBB for status packets
  uint8_t type;         // 0x01: connected, 0x02: disconnected, 0x03: error
  uint8_t device_index; // Device index 0~3
  uint8_t reserved[19]; // Reserved for future use
  uint8_t checksum;     // XOR checksum
} __attribute__((packed)) usb_status_packet_t;

// Device info packet: battery, MAC, charging status, RSSI
// Uses status packet format (0xBB) with type 0x05
// Byte layout: 0xBB | 0x05 | DevIdx | Battery% | MAC[6] | ChgStatus | RSSI | Reserved[10] | XOR
typedef struct {
  uint8_t header;       // 0xBB
  uint8_t type;         // 0x05 = device info
  uint8_t device_index; // Device index 0~3
  uint8_t battery;      // Battery level 0-100, 0xFF = unknown
  uint8_t mac[6];       // BLE MAC address (big-endian)
  uint8_t charging;     // 0=unknown, 1=charging, 2=not charging
  int8_t rssi;          // BLE RSSI in dBm (negative), 0 = unknown
  uint8_t reserved[10]; // Reserved for future use
  uint8_t checksum;     // XOR checksum
} __attribute__((packed)) usb_device_info_packet_t;

// Public API
void usb_cdc_init(uint32_t baudrate = 921600);
void usb_cdc_send_data_packet(uint8_t device_index, uint8_t *ble_data,
                              size_t len);
void usb_cdc_send_status(uint8_t type, uint8_t device_index);
void usb_cdc_send_device_info(uint8_t device_index, int8_t battery,
                              const uint8_t *mac, uint8_t charging,
                              int8_t rssi);
void usb_cdc_process();
bool usb_cdc_is_connected();

// Status types
#define USB_STATUS_CONNECTED 0x01
#define USB_STATUS_DISCONNECTED 0x02
#define USB_STATUS_ERROR 0x03
#define USB_STATUS_STREAMING 0x04
#define USB_STATUS_DEVICE_INFO 0x05

// ============================================================================
// MAC Whitelist Command/Response Packets (PC ↔ Dongle)
// ============================================================================

// Packet headers
#define USB_CMD_HEADER 0xCC  // PC → Dongle command
#define USB_RESP_HEADER 0xDD // Dongle → PC response

// Command types (byte 1 of 0xCC packet)
#define USB_CMD_SET_MAC 0x01   // Set MAC for a slot
#define USB_CMD_QUERY_MAC 0x02 // Query whitelist (single slot or all)
#define USB_CMD_RESET_ALL 0x03 // Reset all slots to wildcard

// Response status codes (byte 2 of 0xDD packet)
#define USB_RESP_ACK 0x00           // Success
#define USB_RESP_NACK_PARAM 0x01    // Parameter error (invalid slot/cmd)
#define USB_RESP_NACK_CHECKSUM 0x02 // Checksum error
#define USB_RESP_NACK_OTHER 0x03    // Other error

// Query all slots marker
#define USB_QUERY_ALL_SLOTS 0xFF

// PC → Dongle: SET_MAC command (23 bytes)
// CC 01 [slot] [MAC×6] [reserved×13] [XOR]
typedef struct {
  uint8_t header;       // 0xCC
  uint8_t cmd;          // 0x01 = SET_MAC
  uint8_t slot;         // 0~3
  uint8_t mac[6];       // MAC address (big-endian)
  uint8_t reserved[13]; // Reserved
  uint8_t checksum;     // XOR of bytes 0~21
} __attribute__((packed)) usb_cmd_set_mac_t;

// PC → Dongle: QUERY_MAC command (23 bytes)
// CC 02 [slot] [reserved×19] [XOR]
typedef struct {
  uint8_t header;       // 0xCC
  uint8_t cmd;          // 0x02 = QUERY_MAC
  uint8_t slot;         // 0~3 single slot, 0xFF = all
  uint8_t reserved[19]; // Reserved
  uint8_t checksum;     // XOR of bytes 0~21
} __attribute__((packed)) usb_cmd_query_mac_t;

// PC → Dongle: RESET_ALL command (23 bytes)
// CC 03 [reserved×20] [XOR]
typedef struct {
  uint8_t header;       // 0xCC
  uint8_t cmd;          // 0x03 = RESET_ALL
  uint8_t reserved[20]; // Reserved
  uint8_t checksum;     // XOR of bytes 0~21
} __attribute__((packed)) usb_cmd_reset_all_t;

// Dongle → PC: Response packet (23 bytes)
// DD [cmd_echo] [status] [slot] [MAC×6] [reserved×11] [XOR]
typedef struct {
  uint8_t header;       // 0xDD
  uint8_t cmd_echo;     // Echo of the command type
  uint8_t status;       // 0x00=ACK, 0x01~0x03=NACK
  uint8_t slot;         // Slot number
  uint8_t mac[6];       // MAC address (big-endian)
  uint8_t reserved[11]; // Reserved
  uint8_t checksum;     // XOR of bytes 0~21
} __attribute__((packed)) usb_cmd_response_t;

#endif // USB_CDC_OUTPUT_H
