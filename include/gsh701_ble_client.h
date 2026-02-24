#ifndef GSH701_BLE_CLIENT_H
#define GSH701_BLE_CLIENT_H

#include <Arduino.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>

// V605 firmware UUIDs (ble_nus.c: FFF0/FFF3/FFF4/FFF5)
#define GSH_SERVICE_UUID_V605 "0000fff0-0000-1000-8000-00805f9b34fb"
#define GSH_NOTIFY_DATA_UUID_V605 "0000fff3-0000-1000-8000-00805f9b34fb"
#define GSH_WRITE_CMD_UUID_V605 "0000fff4-0000-1000-8000-00805f9b34fb"
#define GSH_NOTIFY_RESP_UUID_V605 "0000fff5-0000-1000-8000-00805f9b34fb"

// Legacy firmware UUIDs (FFE0/FFE1/FFE2/FFE3)
#define GSH_SERVICE_UUID_LEGACY "0000ffe0-0000-1000-8000-00805f9b34fb"
#define GSH_NOTIFY_DATA_UUID_LEGACY "0000ffe1-0000-1000-8000-00805f9b34fb"
#define GSH_WRITE_CMD_UUID_LEGACY "0000ffe2-0000-1000-8000-00805f9b34fb"
#define GSH_NOTIFY_RESP_UUID_LEGACY "0000ffe3-0000-1000-8000-00805f9b34fb"

// BLE Battery Service (standard)
#define GSH_BATTERY_SERVICE_UUID "0000180f-0000-1000-8000-00805f9b34fb"
#define GSH_BATTERY_LEVEL_UUID "00002a19-0000-1000-8000-00805f9b34fb"

// Device constraints
#define MAX_GSH_DEVICES 4
#define GSH_PACKET_SIZE 20
#define GSH_AUTH_TIMEOUT_MS 10000   // 10 seconds to send password
#define GSH_RECONNECT_DELAY_MS 5000 // Delay before reconnection attempt
#define GSH_DEVICE_NAME_PREFIX "GSH_751V2"

// Commands (write to FFF4, matching V605 firmware main.c cmd definitions)
#define GSH_CMD_SET_PASSWORD 0xCC // + H_byte + L_byte
#define GSH_CMD_RESEND 0xA5       // + packet number
#define GSH_CMD_DISCONNECT 0xA1   // + 0xC0 to sleep
#define GSH_CMD_ENTER_SLEEP 0xC0  // Second byte for sleep command
#define GSH_CMD_ENTER_OTA 0x5A    // Enter OTA/DFU mode
#define GSH_CMD_256HZ_8BIT 0xF1   // Set 256Hz 8-bit sample rate (+ 0x80 0x08)
#define GSH_CMD_256HZ_16BIT 0xF3  // Set 256Hz 16-bit sample rate (+ 0x80 0x10)

// Device state machine
typedef enum {
  GSH_STATE_DISCONNECTED = 0,
  GSH_STATE_SCANNING,
  GSH_STATE_CONNECTING,
  GSH_STATE_WAIT_FFE3_NOTIFY,
  GSH_STATE_AUTH_PENDING,
  GSH_STATE_DATARATE_PENDING,
  GSH_STATE_WAIT_FFE1_NOTIFY,
  GSH_STATE_STREAMING,
  GSH_STATE_ERROR
} gsh_device_state_t;

// Received packet callback type
typedef void (*gsh_data_callback_t)(uint8_t device_index, uint8_t *data,
                                    size_t len);

// Firmware version detected on device
typedef enum {
  GSH_FW_UNKNOWN = 0,
  GSH_FW_LEGACY, // FFE0/FFE1/FFE2/FFE3
  GSH_FW_V605    // FFF0/FFF3/FFF4/FFF5
} gsh_fw_version_t;

// Device structure
typedef struct {
  uint8_t index;
  BLEAddress *address;
  esp_ble_addr_type_t addr_type; // Address type from advertisement
  gsh_fw_version_t fw_version;   // Detected firmware version
  BLEClient *client;
  BLEClientCallbacks *client_cb; // Persistent callbacks (prevent leak)
  BLERemoteCharacteristic *charFFE1; // RX Notify - ECG data
  BLERemoteCharacteristic *charFFE2; // TX Write - Commands
  BLERemoteCharacteristic *charFFE3; // Resend Notify - Response
  gsh_device_state_t state;
  uint16_t password;
  uint32_t state_enter_time;
  uint32_t last_packet_time;
  uint8_t last_packet_counter;
  bool auth_success;
  bool datarate_success;
  uint8_t auth_retry_count;     // Authentication retry counter
  uint8_t datarate_retry_count; // Data rate setting retry counter
  uint32_t last_retry_time;     // Timestamp of last retry attempt
  uint32_t disconnect_time;     // When device was last disconnected
  uint8_t connect_fail_count;   // Consecutive connection failures
  int8_t battery_level;         // Battery level 0-100%, -1 if unknown
  uint8_t charging_status;      // 0=unknown, 1=charging, 2=not charging
  int8_t rssi;                  // BLE RSSI in dBm, 0 = unknown
} gsh_device_t;

// Public API
void gsh_ble_init();
void gsh_ble_set_data_callback(gsh_data_callback_t callback);
void gsh_ble_start_scan();
void gsh_ble_stop_scan();
bool gsh_connect_device(uint8_t index, BLEAddress *address);
void gsh_disconnect_device(uint8_t index);       // Disconnect without sleep (for reconnection)
void gsh_disconnect_device_sleep(uint8_t index);  // Disconnect + send sleep command to 751
void gsh_disconnect_all();
void gsh_process_all();
uint8_t gsh_get_connected_count();
gsh_device_state_t gsh_get_device_state(uint8_t index);
const char *gsh_state_to_string(gsh_device_state_t state);

// Get device info
bool gsh_is_device_streaming(uint8_t index);
uint32_t gsh_get_last_packet_time(uint8_t index);
int8_t gsh_get_battery_level(uint8_t index);
uint8_t gsh_get_charging_status(uint8_t index);
int8_t gsh_get_rssi(uint8_t index);
bool gsh_get_mac_address(uint8_t index, uint8_t *out_mac);

#endif // GSH701_BLE_CLIENT_H
