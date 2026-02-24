#include "usb_cdc_output.h"
#include "gsh701_ble_client.h"
#include "mac_whitelist.h"

// GSH packet size (defined here to avoid circular dependency)
#ifndef GSH_PACKET_SIZE
#define GSH_PACKET_SIZE 20
#endif

// ============================================================================
// Native USB CDC instance for data output
// ============================================================================
USBCDC USBData(0);

// ============================================================================
// Static variables
// ============================================================================
static bool usb_initialized = false;

// Receive buffer for incoming commands from PC
static uint8_t rx_buf[USB_PACKET_TOTAL_SIZE];
static uint8_t rx_pos = 0;

// ============================================================================
// Public API Implementation
// ============================================================================

void usb_cdc_init(uint32_t baudrate) {
  // Initialize native USB CDC for data output (separate from debug UART)
  USBData.begin(baudrate);
  USB.begin();

  // Wait for native USB CDC to connect
  uint32_t start = millis();
  while (!USBData && (millis() - start) < 3000) {
    delay(10);
  }

  usb_initialized = true;
  Serial.println("[USB] Native USB CDC data port initialized");
}

void usb_cdc_send_data_packet(uint8_t device_index, uint8_t *ble_data,
                              size_t len) {
  if (!usb_initialized || device_index >= 4 || len < GSH_PACKET_SIZE) {
    return;
  }

  usb_packet_t packet;

  // Fill packet structure
  packet.header = USB_HEADER_BYTE;
  packet.device_index = device_index;
  packet.status =
      ble_data[0];           // Byte0: bit7=sensor on/off, bit0-4=packet counter
  packet.code = ble_data[1]; // Byte1: Code (0x80)

  // Copy raw data (Byte2~19 from BLE packet)
  memcpy(packet.data, &ble_data[2], 18);

  // Calculate XOR checksum
  uint8_t checksum = 0;
  uint8_t *pkt_bytes = (uint8_t *)&packet;
  for (int i = 0; i < sizeof(usb_packet_t) - 1; i++) {
    checksum ^= pkt_bytes[i];
  }
  packet.checksum = checksum;

  // Send packet via native USB CDC
  USBData.write((uint8_t *)&packet, sizeof(usb_packet_t));
}

void usb_cdc_send_status(uint8_t type, uint8_t device_index) {
  if (!usb_initialized) {
    return;
  }

  usb_status_packet_t packet;
  memset(&packet, 0, sizeof(packet));

  packet.header = 0xBB; // Status packet header
  packet.type = type;
  packet.device_index = device_index;

  // Calculate XOR checksum
  uint8_t checksum = 0;
  uint8_t *pkt_bytes = (uint8_t *)&packet;
  for (int i = 0; i < sizeof(usb_status_packet_t) - 1; i++) {
    checksum ^= pkt_bytes[i];
  }
  packet.checksum = checksum;

  // Send status packet via native USB CDC
  USBData.write((uint8_t *)&packet, sizeof(usb_status_packet_t));
}

void usb_cdc_send_device_info(uint8_t device_index, int8_t battery,
                              const uint8_t *mac, uint8_t charging,
                              int8_t rssi) {
  if (!usb_initialized) {
    return;
  }

  usb_device_info_packet_t packet;
  memset(&packet, 0, sizeof(packet));

  packet.header = 0xBB;
  packet.type = USB_STATUS_DEVICE_INFO;
  packet.device_index = device_index;
  packet.battery = (battery >= 0) ? (uint8_t)battery : 0xFF;
  if (mac != nullptr) {
    memcpy(packet.mac, mac, 6);
  }
  packet.charging = charging;
  packet.rssi = rssi;

  // Calculate XOR checksum
  uint8_t checksum = 0;
  uint8_t *pkt_bytes = (uint8_t *)&packet;
  for (size_t i = 0; i < sizeof(usb_device_info_packet_t) - 1; i++) {
    checksum ^= pkt_bytes[i];
  }
  packet.checksum = checksum;

  USBData.write((uint8_t *)&packet, sizeof(usb_device_info_packet_t));
}

// ============================================================================
// Command Processing Helpers
// ============================================================================

static void send_response(uint8_t cmd_echo, uint8_t status, uint8_t slot,
                          const uint8_t *mac) {
  usb_cmd_response_t resp;
  memset(&resp, 0, sizeof(resp));

  resp.header = USB_RESP_HEADER;
  resp.cmd_echo = cmd_echo;
  resp.status = status;
  resp.slot = slot;
  if (mac != nullptr) {
    memcpy(resp.mac, mac, 6);
  }

  // Calculate XOR checksum
  uint8_t checksum = 0;
  uint8_t *p = (uint8_t *)&resp;
  for (size_t i = 0; i < sizeof(resp) - 1; i++) {
    checksum ^= p[i];
  }
  resp.checksum = checksum;

  USBData.write((uint8_t *)&resp, sizeof(resp));
}

static void handle_set_mac(const uint8_t *pkt) {
  uint8_t slot = pkt[2];
  const uint8_t *mac = &pkt[3];

  if (slot >= WHITELIST_MAX_SLOTS) {
    send_response(USB_CMD_SET_MAC, USB_RESP_NACK_PARAM, slot, nullptr);
    return;
  }

  if (whitelist_set_mac(slot, mac)) {
    send_response(USB_CMD_SET_MAC, USB_RESP_ACK, slot, mac);
  } else {
    send_response(USB_CMD_SET_MAC, USB_RESP_NACK_OTHER, slot, mac);
    return;
  }

  // Disconnect all and restart scan so whitelist takes effect
  Serial.println("[USB] MAC set — disconnecting all, restarting scan");
  gsh_disconnect_all();
  gsh_ble_start_scan();
}

static void handle_query_mac(const uint8_t *pkt) {
  uint8_t slot = pkt[2];

  if (slot == USB_QUERY_ALL_SLOTS) {
    // Return all 4 slots
    for (uint8_t i = 0; i < WHITELIST_MAX_SLOTS; i++) {
      uint8_t mac[6];
      whitelist_get_mac(i, mac);
      send_response(USB_CMD_QUERY_MAC, USB_RESP_ACK, i, mac);
    }
  } else if (slot < WHITELIST_MAX_SLOTS) {
    uint8_t mac[6];
    whitelist_get_mac(slot, mac);
    send_response(USB_CMD_QUERY_MAC, USB_RESP_ACK, slot, mac);
  } else {
    send_response(USB_CMD_QUERY_MAC, USB_RESP_NACK_PARAM, slot, nullptr);
  }
}

static void handle_reset_all() {
  whitelist_reset_all();
  send_response(USB_CMD_RESET_ALL, USB_RESP_ACK, 0, nullptr);

  // Disconnect all and restart scan
  Serial.println("[USB] Whitelist reset — disconnecting all, restarting scan");
  gsh_disconnect_all();
  gsh_ble_start_scan();
}

void usb_cdc_process() {
  // Read available bytes into rx_buf
  while (USBData.available() && rx_pos < USB_PACKET_TOTAL_SIZE) {
    rx_buf[rx_pos++] = USBData.read();
  }

  // Not enough data yet
  if (rx_pos < USB_PACKET_TOTAL_SIZE) {
    return;
  }

  // We have 23 bytes — check header
  if (rx_buf[0] != USB_CMD_HEADER) {
    // Not a valid command — shift buffer by 1 to re-sync
    memmove(rx_buf, rx_buf + 1, rx_pos - 1);
    rx_pos--;
    return;
  }

  // Verify XOR checksum (all 23 bytes XOR should be 0)
  uint8_t xor_check = 0;
  for (int i = 0; i < USB_PACKET_TOTAL_SIZE; i++) {
    xor_check ^= rx_buf[i];
  }

  if (xor_check != 0) {
    Serial.println("[USB] Command checksum error");
    send_response(rx_buf[1], USB_RESP_NACK_CHECKSUM, 0, nullptr);
    rx_pos = 0;
    return;
  }

  // Dispatch command
  uint8_t cmd = rx_buf[1];
  switch (cmd) {
  case USB_CMD_SET_MAC:
    handle_set_mac(rx_buf);
    break;
  case USB_CMD_QUERY_MAC:
    handle_query_mac(rx_buf);
    break;
  case USB_CMD_RESET_ALL:
    handle_reset_all();
    break;
  default:
    Serial.printf("[USB] Unknown command: 0x%02X\n", cmd);
    send_response(cmd, USB_RESP_NACK_PARAM, 0, nullptr);
    break;
  }

  // Reset buffer
  rx_pos = 0;
}

bool usb_cdc_is_connected() { return usb_initialized && USBData; }

// ============================================================================
// Packet size definition (20 bytes BLE -> 23 bytes USB)
// ============================================================================
// BLE Packet (20 bytes):
//   Byte0: bit7=sensor on/off, bit0-4=packet counter, bit5-6=reserved
//   Byte1: Code (0x80)
//   Byte2-19: Raw ECG data
//
// USB Packet (23 bytes):
//   Byte0: Header (0xAA)
//   Byte1: Device Index (0-3)
//   Byte2: Status (same as BLE Byte0)
//   Byte3: Code (same as BLE Byte1)
//   Byte4-21: Data (same as BLE Byte2-19)
//   Byte22: XOR Checksum
