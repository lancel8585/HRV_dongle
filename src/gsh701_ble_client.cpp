#include "gsh701_ble_client.h"
#include "mac_whitelist.h"

// ============================================================================
// Debug configuration
// ============================================================================
#define GSH_DEBUG 1 // Set to 0 to disable debug messages

#if GSH_DEBUG
#define GSH_LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define GSH_LOG(fmt, ...) ((void)0)
#endif

// ============================================================================
// Constants
// ============================================================================
#define GSH_MAX_AUTH_RETRIES 10
#define GSH_MAX_DATARATE_RETRIES 5
#define GSH_RETRY_INTERVAL_MS 1000

// ============================================================================
// Static variables
// ============================================================================
static gsh_device_t gsh_devices[MAX_GSH_DEVICES];
static BLEScan *pBLEScan = nullptr;
static gsh_data_callback_t data_callback = nullptr;
static bool is_scanning = false;
static bool has_pending_devices =
    false; // Flag: devices found during scan, connect after scan ends

// Blacklist: devices that connected but had no GSH service (e.g. GSH288)
#define GSH_BLACKLIST_SIZE 8
#define GSH_BLACKLIST_TIMEOUT_MS (5 * 60 * 1000) // 5 minutes

static struct {
  uint8_t mac[6];
  uint32_t fail_time;
  bool active;
} ble_blacklist[GSH_BLACKLIST_SIZE];

static void blacklist_add(const uint8_t *mac) {
  uint32_t now = millis();
  // Update timestamp if already listed
  for (int i = 0; i < GSH_BLACKLIST_SIZE; i++) {
    if (ble_blacklist[i].active && memcmp(ble_blacklist[i].mac, mac, 6) == 0) {
      ble_blacklist[i].fail_time = now;
      return;
    }
  }
  // Find free or expired slot
  int slot = -1;
  for (int i = 0; i < GSH_BLACKLIST_SIZE; i++) {
    if (!ble_blacklist[i].active ||
        (now - ble_blacklist[i].fail_time) > GSH_BLACKLIST_TIMEOUT_MS) {
      slot = i;
      break;
    }
  }
  // Evict oldest if all slots full
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < GSH_BLACKLIST_SIZE; i++) {
      if (ble_blacklist[i].fail_time < ble_blacklist[slot].fail_time)
        slot = i;
    }
  }
  memcpy(ble_blacklist[slot].mac, mac, 6);
  ble_blacklist[slot].fail_time = now;
  ble_blacklist[slot].active = true;
  GSH_LOG("[BL] Blacklisted: %02X:%02X:%02X:%02X:%02X:%02X (5min)\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool blacklist_check(const uint8_t *mac) {
  uint32_t now = millis();
  for (int i = 0; i < GSH_BLACKLIST_SIZE; i++) {
    if (ble_blacklist[i].active && memcmp(ble_blacklist[i].mac, mac, 6) == 0) {
      if ((now - ble_blacklist[i].fail_time) < GSH_BLACKLIST_TIMEOUT_MS)
        return true; // Still blacklisted
      ble_blacklist[i].active = false; // Expired
    }
  }
  return false;
}

// Forward declarations
static void gsh_ble_scan_on_complete(BLEScanResults foundDevices);
static void gsh_disconnect_device_internal(uint8_t index, bool send_sleep);
static uint16_t calculatePassword(BLEAddress *address);
static bool sendPassword(gsh_device_t *device);
static bool sendDataRate(gsh_device_t *device, uint8_t rate);
static void changeState(gsh_device_t *device, gsh_device_state_t newState);

// ============================================================================
// BLE Client Callbacks
// ============================================================================
class GshClientCallbacks : public BLEClientCallbacks {
public:
  uint8_t deviceIndex;

  GshClientCallbacks(uint8_t idx) : deviceIndex(idx) {}

  void onConnect(BLEClient *pClient) override {
    GSH_LOG("[GSH%d] Connected\n", deviceIndex);
  }

  void onDisconnect(BLEClient *pClient) override {
    gsh_device_t *device = &gsh_devices[deviceIndex];
    GSH_LOG("[GSH%d] Disconnected (callback), was in state: %s\n", deviceIndex,
            gsh_state_to_string(device->state));

    // Clean up allocated address to prevent memory leak
    if (device->address != nullptr) {
      delete device->address;
      device->address = nullptr;
    }

    // Clear characteristic pointers (invalidated after disconnect)
    device->charFFE1 = nullptr;
    device->charFFE2 = nullptr;
    device->charFFE3 = nullptr;

    device->auth_success = false;
    device->datarate_success = false;
    device->auth_retry_count = 0;
    device->datarate_retry_count = 0;
    device->last_retry_time = 0;
    device->last_packet_time = 0;

    // Only set disconnect_time if not already set by gsh_disconnect_device_internal
    if (device->disconnect_time == 0) {
      device->disconnect_time = millis();
    }

    changeState(device, GSH_STATE_DISCONNECTED);
  }
};

// ============================================================================
// BLE Scan Callbacks
// ============================================================================
class GshAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    // Check if device name matches GSH_ECG
    std::string devName =
        advertisedDevice.haveName() ? advertisedDevice.getName() : "N/A";
    GSH_LOG("[SCAN] Found: %s (%s) RSSI: %d, SRV: %s\n", devName.c_str(),
            advertisedDevice.getAddress().toString().c_str(),
            advertisedDevice.getRSSI(),
            advertisedDevice.haveServiceUUID()
                ? advertisedDevice.getServiceUUID().toString().c_str()
                : "None");

    bool matchName = advertisedDevice.haveName() &&
                     advertisedDevice.getName().find(GSH_DEVICE_NAME_PREFIX) !=
                         std::string::npos;
    // V605 firmware advertises manufacturer data with company ID 0x59
    // (GSHCARE). The advertising packet is too large for the device name to
    // fit, so we match by manufacturer data instead.
    bool matchManuf = false;
    if (advertisedDevice.haveManufacturerData()) {
      std::string mfData = advertisedDevice.getManufacturerData();
      // Company ID is first 2 bytes (little-endian): 0x59 0x00
      if (mfData.length() >= 2 && (uint8_t)mfData[0] == 0x59 &&
          (uint8_t)mfData[1] == 0x00) {
        matchManuf = true;
      }
    }

    // UUID alone is not enough — other devices (e.g. RINGOAL_HOME) also use
    // FFF0. Require GSH name prefix or GSHCARE manufacturer data (0x59).
    if (matchName || matchManuf) {

      // Check blacklist (devices that connected but had no GSH service)
      if (blacklist_check(*advertisedDevice.getAddress().getNative())) {
        GSH_LOG("[SCAN] Skipped %s (blacklisted)\n",
                advertisedDevice.getAddress().toString().c_str());
        return;
      }

      // Check if we have room for more devices and not already connected
      uint8_t connected = gsh_get_connected_count();
      if (connected < MAX_GSH_DEVICES) {
        // Check if this device is already known
        BLEAddress addr = advertisedDevice.getAddress();
        bool already_known = false;
        int free_slot = -1;

        for (int i = 0; i < MAX_GSH_DEVICES; i++) {
          if (gsh_devices[i].state != GSH_STATE_DISCONNECTED) {
            if (gsh_devices[i].address && *gsh_devices[i].address == addr) {
              already_known = true;
              break;
            }
          } else if (free_slot < 0) {
            free_slot = i;
          }
        }

        if (!already_known && free_slot >= 0) {
          // Whitelist check: extract big-endian MAC and verify
          const uint8_t *native = *addr.getNative();
          uint8_t mac_be[6];
          for (int j = 0; j < 6; j++) {
            mac_be[j] = native[5 - j];
          }
          if (!whitelist_is_allowed(mac_be)) {
            GSH_LOG("[SCAN] Skipped %s (not in whitelist)\n",
                    addr.toString().c_str());
            return;
          }

          // Bind-priority: if this MAC is only allowed by wildcard slots,
          // check whether the wildcard quota is already full.
          if (!whitelist_is_exact_match(mac_be)) {
            uint8_t bound_count = whitelist_get_bound_count();
            uint8_t max_wildcard = MAX_GSH_DEVICES - bound_count;

            // Count how many currently occupied device slots are wildcard devices
            uint8_t wildcard_occupied = 0;
            for (int j = 0; j < MAX_GSH_DEVICES; j++) {
              if (gsh_devices[j].state != GSH_STATE_DISCONNECTED
                  && gsh_devices[j].address != nullptr) {
                const uint8_t *nat = *gsh_devices[j].address->getNative();
                uint8_t dev_mac[6];
                for (int k = 0; k < 6; k++) dev_mac[k] = nat[5 - k];
                if (!whitelist_is_exact_match(dev_mac)) {
                  wildcard_occupied++;
                }
              }
            }

            if (wildcard_occupied >= max_wildcard) {
              GSH_LOG("[SCAN] Skipped %s (wildcard full %d/%d)\n",
                      addr.toString().c_str(), wildcard_occupied, max_wildcard);
              return;
            }
          }

          GSH_LOG("[SCAN] Found GSH_ECG device: %s (RSSI: %d)\n",
                  addr.toString().c_str(), advertisedDevice.getRSSI());

          // Store address, address type, and RSSI from scan result
          gsh_devices[free_slot].address = new BLEAddress(addr);
          gsh_devices[free_slot].addr_type =
              advertisedDevice.getAddressType();
          gsh_devices[free_slot].rssi = (int8_t)advertisedDevice.getRSSI();
          changeState(&gsh_devices[free_slot], GSH_STATE_CONNECTING);
          has_pending_devices = true;

          // Stop scan early so we can connect sooner
          pBLEScan->stop();
          pBLEScan->clearResults();
          is_scanning = false;
        }
      }
    }
  }
};

// ============================================================================
// Public API Implementation
// ============================================================================

// Static callback instance to prevent memory leak
static GshAdvertisedDeviceCallbacks scanCallbacks;

void gsh_ble_init() {
  GSH_LOG("[BLE] Initializing BLE for GSH_701...\n");

  GSH_LOG("[BLE] Calling BLEDevice::init...\n");
  BLEDevice::init("HRV_Dongle");
  GSH_LOG("[BLE] BLEDevice::init complete\n");

  // Initialize device structures
  GSH_LOG("[BLE] Initializing device structures...\n");
  for (int i = 0; i < MAX_GSH_DEVICES; i++) {
    gsh_devices[i].index = i;
    gsh_devices[i].address = nullptr;
    gsh_devices[i].addr_type = BLE_ADDR_TYPE_PUBLIC;
    gsh_devices[i].fw_version = GSH_FW_UNKNOWN;
    gsh_devices[i].client = nullptr;
    gsh_devices[i].client_cb = new GshClientCallbacks(i);
    gsh_devices[i].charFFE1 = nullptr;
    gsh_devices[i].charFFE2 = nullptr;
    gsh_devices[i].charFFE3 = nullptr;
    gsh_devices[i].state = GSH_STATE_DISCONNECTED;
    gsh_devices[i].password = 0;
    gsh_devices[i].state_enter_time = 0;
    gsh_devices[i].last_packet_time = 0;
    gsh_devices[i].last_packet_counter = 0;
    gsh_devices[i].auth_success = false;
    gsh_devices[i].datarate_success = false;
    gsh_devices[i].auth_retry_count = 0;
    gsh_devices[i].datarate_retry_count = 0;
    gsh_devices[i].last_retry_time = 0;
    gsh_devices[i].disconnect_time = 0;
    gsh_devices[i].connect_fail_count = 0;
    gsh_devices[i].battery_level = -1;
    gsh_devices[i].charging_status = 0;
    gsh_devices[i].rssi = 0;
  }

  // Setup BLE scanner (use static callback to avoid memory leak)
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(&scanCallbacks);
  pBLEScan->setActiveScan(
      true); // Active scan uses more power, but gets results faster
  pBLEScan->setInterval(40); // 40 * 0.625ms = 25ms interval
  pBLEScan->setWindow(40);   // 40 * 0.625ms = 25ms window (100% duty cycle)

  GSH_LOG("[BLE] Initialization complete\n");
}

void gsh_ble_set_data_callback(gsh_data_callback_t callback) {
  data_callback = callback;
}

void gsh_ble_start_scan() {
  if (!is_scanning) {
    uint8_t connected = gsh_get_connected_count();
    int scan_secs;
    if (connected > 0) {
      // Reduce scan duty cycle when devices are connected to avoid
      // starving BLE connection events (notifications drop otherwise).
      // 10% duty cycle: interval=160 (100ms), window=16 (10ms)
      pBLEScan->setInterval(160);
      pBLEScan->setWindow(16);
      scan_secs = 3;
    } else {
      // No connections: aggressive scan for fast discovery
      // 100% duty cycle: interval=40 (25ms), window=40 (25ms)
      pBLEScan->setInterval(40);
      pBLEScan->setWindow(40);
      scan_secs = 5;
    }
    GSH_LOG("[SCAN] Starting BLE scan (%ds, %s duty)...\n",
            scan_secs, connected > 0 ? "low" : "full");
    is_scanning = true;
    pBLEScan->clearResults();
    pBLEScan->start(scan_secs, gsh_ble_scan_on_complete, false);
  }
}

void gsh_ble_stop_scan() {
  if (is_scanning) {
    GSH_LOG("[SCAN] Stopping BLE scan\n");
    pBLEScan->stop();
    pBLEScan->clearResults(); // Clear cache to find devices again
    is_scanning = false;
  }
}

// Callback when scan ends
void gsh_ble_scan_on_complete(BLEScanResults foundDevices) {
  GSH_LOG("[SCAN] Scan complete! Found %d devices\n", foundDevices.getCount());
  pBLEScan
      ->clearResults(); // Delete results from BLEScan buffer to release memory
  is_scanning = false;
  // Pending connections will be handled by gsh_process_all loop
}

bool gsh_connect_device(uint8_t index, BLEAddress *address) {
  if (index >= MAX_GSH_DEVICES)
    return false;

  gsh_device_t *device = &gsh_devices[index];

  // Create BLE client if needed, or reuse existing (after disconnect)
  if (device->client == nullptr) {
    device->client = BLEDevice::createClient();
    device->client->setClientCallbacks(device->client_cb);
    GSH_LOG("[GSH%d] Created new BLE client\n", index);
  } else {
    GSH_LOG("[GSH%d] Reusing existing BLE client\n", index);
  }

  GSH_LOG("[GSH%d] Connecting to %s...\n", index, address->toString().c_str());

  // Stop scanning before connecting to avoid BLE stack conflicts
  if (is_scanning) {
    GSH_LOG("[GSH%d] Stopping scan before connect...\n", index);
    pBLEScan->stop();
    pBLEScan->clearResults();
    is_scanning = false;
    delay(200); // Give BLE stack time to settle
  }

  // Use address type stored from advertisement scan result
  esp_ble_addr_type_t addrType = device->addr_type;
  GSH_LOG("[GSH%d] Using address type: %s (%d)\n", index,
          addrType == BLE_ADDR_TYPE_PUBLIC  ? "PUBLIC"
          : addrType == BLE_ADDR_TYPE_RANDOM ? "RANDOM"
                                             : "OTHER",
          addrType);

  if (!device->client->connect(*address, addrType)) {
    GSH_LOG("[GSH%d] Connection failed (attempt %d)\n", index,
            device->connect_fail_count + 1);
    device->connect_fail_count++;
    // On connect failure, BLE library cleans up internally.
    // Delete and recreate client to ensure clean state for next attempt.
    delete device->client;
    device->client = nullptr;
    device->disconnect_time = millis(); // Set cooldown timer
    changeState(device, GSH_STATE_DISCONNECTED);
    return false;
  }

  // Connection succeeded, reset failure counter
  device->connect_fail_count = 0;

  // Try V605 service (0xFFF0) first, then legacy (0xFFE0)
  BLERemoteService *pService =
      device->client->getService(BLEUUID(GSH_SERVICE_UUID_V605));
  if (pService != nullptr) {
    device->fw_version = GSH_FW_V605;
    GSH_LOG("[GSH%d] Service 0xFFF0 found (V605 firmware)\n", index);
    device->charFFE1 =
        pService->getCharacteristic(BLEUUID(GSH_NOTIFY_DATA_UUID_V605));
    device->charFFE2 =
        pService->getCharacteristic(BLEUUID(GSH_WRITE_CMD_UUID_V605));
    device->charFFE3 =
        pService->getCharacteristic(BLEUUID(GSH_NOTIFY_RESP_UUID_V605));
  } else {
    pService =
        device->client->getService(BLEUUID(GSH_SERVICE_UUID_LEGACY));
    if (pService != nullptr) {
      device->fw_version = GSH_FW_LEGACY;
      GSH_LOG("[GSH%d] Service 0xFFE0 found (legacy firmware)\n", index);
      device->charFFE1 =
          pService->getCharacteristic(BLEUUID(GSH_NOTIFY_DATA_UUID_LEGACY));
      device->charFFE2 =
          pService->getCharacteristic(BLEUUID(GSH_WRITE_CMD_UUID_LEGACY));
      device->charFFE3 =
          pService->getCharacteristic(BLEUUID(GSH_NOTIFY_RESP_UUID_LEGACY));
    } else {
      GSH_LOG("[GSH%d] No GSH service found (tried FFF0 and FFE0)\n", index);
      blacklist_add(*address->getNative());
      device->client->disconnect();
      changeState(device, GSH_STATE_DISCONNECTED);
      return false;
    }
  }

  if (device->charFFE1 == nullptr || device->charFFE2 == nullptr) {
    GSH_LOG("[GSH%d] Required characteristics not found (Data=%p Write=%p)\n",
            index, device->charFFE1, device->charFFE2);
    device->client->disconnect();
    changeState(device, GSH_STATE_DISCONNECTED);
    return false;
  }
  if (device->charFFE3 == nullptr) {
    GSH_LOG("[GSH%d] Response characteristic not found, will proceed without "
            "it\n",
            index);
  }
  GSH_LOG("[GSH%d] Characteristics found (FFF3=%p FFF4=%p FFF5=%p)\n", index,
          device->charFFE1, device->charFFE2, device->charFFE3);

  // Try to read battery level from standard Battery Service (0x180F)
  device->battery_level = -1;
  BLERemoteService *pBattService =
      device->client->getService(BLEUUID(GSH_BATTERY_SERVICE_UUID));
  if (pBattService != nullptr) {
    BLERemoteCharacteristic *pBattChar =
        pBattService->getCharacteristic(BLEUUID(GSH_BATTERY_LEVEL_UUID));
    if (pBattChar != nullptr && pBattChar->canRead()) {
      uint8_t battVal = pBattChar->readUInt8();
      device->battery_level = (int8_t)battVal;
      GSH_LOG("[GSH%d] Battery level: %d%%\n", index, battVal);
    } else {
      GSH_LOG("[GSH%d] Battery characteristic not readable\n", index);
    }
  } else {
    GSH_LOG("[GSH%d] No Battery Service (0x180F) found\n", index);
  }

  // Calculate password from MAC address
  device->password = calculatePassword(address);
  GSH_LOG("[GSH%d] Calculated password: 0x%04X\n", index, device->password);

  // Step 1: Enable FFF5 notify first (for resend/response)
  changeState(device, GSH_STATE_WAIT_FFE3_NOTIFY);

  if (device->charFFE3 != nullptr && device->charFFE3->canNotify()) {
    // Lambda to capture device index
    device->charFFE3->registerForNotify([index](BLERemoteCharacteristic *pChar,
                                                uint8_t *pData, size_t length,
                                                bool isNotify) {
      gsh_device_t *dev = &gsh_devices[index];

      // Parse response - V605 firmware echoes command byte as first byte:
      //   Success: {CMD, 0x4F, 0x4B} ("OK")
      //   Failure: {CMD, 0x55, 0xAA}
      //   Status:  {'C','H','G', ...} periodic status (11-12 bytes)
      if (length >= 3) {
        // Check for OK response (CMD_ECHO, 0x4F, 0x4B = "OK")
        if (pData[1] == 0x4F && pData[2] == 0x4B) {
          GSH_LOG("[GSH%d] FFF5 OK: cmd=0x%02X\n", index, pData[0]);
          if (dev->state == GSH_STATE_AUTH_PENDING) {
            dev->auth_success = true;
            GSH_LOG("[GSH%d] Authentication successful (OK)\n", index);
          } else if (dev->state == GSH_STATE_DATARATE_PENDING) {
            dev->datarate_success = true;
            GSH_LOG("[GSH%d] Data rate set successfully (OK)\n", index);
          }
        }
        // Check for FAIL response (CMD_ECHO, 0x55, 0xAA)
        else if (pData[1] == 0x55 && pData[2] == 0xAA) {
          GSH_LOG("[GSH%d] FFF5 FAIL: cmd=0x%02X\n", index, pData[0]);
          if (dev->state == GSH_STATE_AUTH_PENDING) {
            GSH_LOG("[GSH%d] Authentication failed\n", index);
          } else if (dev->state == GSH_STATE_DATARATE_PENDING) {
            GSH_LOG("[GSH%d] Data rate setting failed\n", index);
          }
        }
        // Periodic status message (e.g. "CHG:Charging" or "CHG:NoPower")
        else if (length >= 7 && pData[0] == 0x43 && pData[1] == 0x48 &&
                 pData[2] == 0x47 && pData[3] == 0x3A) {
          // "CHG:" prefix — parse charging status
          // "CHG:Charging" (12 bytes) = charging
          // "CHG:NoPower"  (11 bytes) = not charging
          if (length >= 12 && pData[4] == 'C') {
            dev->charging_status = 1; // Charging
          } else if (length >= 11 && pData[4] == 'N') {
            dev->charging_status = 2; // NoPower (not charging)
          }
        }
        // Other unknown status messages
        else if (length >= 4) {
          GSH_LOG("[GSH%d] FFF5 Unknown (%d): ", index, length);
          for (size_t j = 0; j < length && j < 20; j++) {
            GSH_LOG("%02X ", pData[j]);
          }
          GSH_LOG("\n");
        }
      } else if (length >= 1) {
        GSH_LOG("[GSH%d] FFF5 short response: len=%d data[0]=0x%02X\n", index,
                length, pData[0]);
      }
    });
    GSH_LOG("[GSH%d] FFF5 Notify enabled\n", index);
  }

  changeState(device, GSH_STATE_AUTH_PENDING);
  return true;
}

void gsh_disconnect_device(uint8_t index) {
  gsh_disconnect_device_internal(index, false);
}

void gsh_disconnect_device_sleep(uint8_t index) {
  gsh_disconnect_device_internal(index, true);
}

static void gsh_disconnect_device_internal(uint8_t index, bool send_sleep) {
  if (index >= MAX_GSH_DEVICES)
    return;

  gsh_device_t *device = &gsh_devices[index];

  if (device->client != nullptr) {
    if (device->client->isConnected()) {
      // Only send sleep command on explicit user-requested disconnect
      if (send_sleep && device->charFFE2 != nullptr) {
        GSH_LOG("[GSH%d] Sending sleep command before disconnect\n", index);
        uint8_t cmd[] = {GSH_CMD_DISCONNECT, GSH_CMD_ENTER_SLEEP};
        device->charFFE2->writeValue(cmd, sizeof(cmd));
        delay(100);
      }
      device->client->disconnect();
    }
    // Do NOT delete client here — the onDisconnect callback fires
    // asynchronously from the BLE task. Deleting now causes use-after-free
    // crash which reboots ESP32 and drops the USB connection to PC.
    // The client will be reused on next connection attempt.
  }

  // Clear characteristic pointers (invalidated after disconnect)
  device->charFFE1 = nullptr;
  device->charFFE2 = nullptr;
  device->charFFE3 = nullptr;

  device->disconnect_time = millis();
  changeState(device, GSH_STATE_DISCONNECTED);
}

void gsh_disconnect_all() {
  for (int i = 0; i < MAX_GSH_DEVICES; i++) {
    gsh_disconnect_device(i);
  }
}

void gsh_process_all() {
  uint32_t now = millis();

  for (int i = 0; i < MAX_GSH_DEVICES; i++) {
    gsh_device_t *device = &gsh_devices[i];
    uint32_t elapsed = now - device->state_enter_time;

    switch (device->state) {
    case GSH_STATE_DISCONNECTED:
      // Nothing to do
      break;

    case GSH_STATE_CONNECTING:
      // Wait for scan to finish before attempting connection
      if (is_scanning) {
        break;
      }
      // Initiate connection
      if (device->address != nullptr) {
        if (!gsh_connect_device(i, device->address)) {
          // Connection failed, clean up
          delete device->address;
          device->address = nullptr;
        }
      }
      break;

    case GSH_STATE_AUTH_PENDING:
      if (!device->auth_success) {
        // Check retry count limit
        if (device->auth_retry_count >= GSH_MAX_AUTH_RETRIES) {
          if (device->charFFE3 == nullptr) {
            // No FFF5 response channel — assume auth accepted after retries
            GSH_LOG("[GSH%d] No FFF5, assuming auth success after %d sends\n",
                    i, device->auth_retry_count);
            device->auth_success = true;
          } else {
            GSH_LOG("[GSH%d] Authentication max retries exceeded\n", i);
            gsh_disconnect_device(i);
          }
        } else if (elapsed < GSH_AUTH_TIMEOUT_MS) {
          // Use timestamp-based retry instead of modulo
          if ((now - device->last_retry_time) >= GSH_RETRY_INTERVAL_MS) {
            device->last_retry_time = now;
            device->auth_retry_count++;
            sendPassword(device);
          }
        } else {
          GSH_LOG("[GSH%d] Authentication timeout\n", i);
          gsh_disconnect_device(i);
        }
      } else {
        // Auth success, move to data rate setting
        device->last_retry_time = 0; // Reset for next stage
        changeState(device, GSH_STATE_DATARATE_PENDING);
      }
      break;

    case GSH_STATE_DATARATE_PENDING:
      if (!device->datarate_success) {
        // Check retry count limit
        if (device->datarate_retry_count >= GSH_MAX_DATARATE_RETRIES) {
          if (device->charFFE3 == nullptr) {
            // No FFF5 response channel — assume data rate accepted
            GSH_LOG("[GSH%d] No FFF5, assuming data rate success after %d "
                    "sends\n",
                    i, device->datarate_retry_count);
            device->datarate_success = true;
          } else {
            GSH_LOG("[GSH%d] Data rate setting max retries exceeded\n", i);
            gsh_disconnect_device(i);
          }
        } else if (elapsed < 5000) {
          // Use timestamp-based retry
          if ((now - device->last_retry_time) >= GSH_RETRY_INTERVAL_MS) {
            device->last_retry_time = now;
            device->datarate_retry_count++;
            sendDataRate(device, GSH_CMD_512HZ_16BIT);
          }
        } else {
          GSH_LOG("[GSH%d] Data rate setting timeout\n", i);
          gsh_disconnect_device(i);
        }
      } else {
        changeState(device, GSH_STATE_WAIT_FFE1_NOTIFY);
      }
      break;

    case GSH_STATE_WAIT_FFE1_NOTIFY:
      // Enable FFF3 notify for ECG data streaming
      if (device->charFFE1 != nullptr && device->charFFE1->canNotify()) {
        device->charFFE1->registerForNotify([i](BLERemoteCharacteristic *pChar,
                                                uint8_t *pData, size_t length,
                                                bool isNotify) {
          gsh_devices[i].last_packet_time = millis();
          gsh_devices[i].last_packet_counter = pData[0] & 0x1F;

          // Call data callback
          if (data_callback != nullptr) {
            data_callback(i, pData, length);
          }
        });
        GSH_LOG("[GSH%d] FFF3 Data Notify enabled - Streaming started\n", i);
        // Reset packet timestamp so data timeout check starts fresh
        // (prevents immediate timeout from stale value after reconnect)
        device->last_packet_time = millis();
        changeState(device, GSH_STATE_STREAMING);
      }
      break;

    case GSH_STATE_STREAMING:
      // Check for connection timeout (no data for 5 seconds)
      if (device->last_packet_time > 0 &&
          (now - device->last_packet_time) > 5000) {
        GSH_LOG("[GSH%d] Data timeout, reconnecting...\n", i);
        gsh_disconnect_device(i);
      }
      // RSSI is stored from scan result (not polled during streaming
      // to avoid BLE stack conflicts with other device operations)
      break;

    case GSH_STATE_ERROR:
      // Wait and retry
      if (elapsed > GSH_RECONNECT_DELAY_MS) {
        gsh_disconnect_device(i);
      }
      break;

    default:
      break;
    }
  }

  // Auto-start scan loop — with cooldown after disconnect
  static uint32_t last_scan_check = 0;

  if (gsh_get_connected_count() < MAX_GSH_DEVICES) {
    if (!is_scanning) {
      // Check if any device was recently disconnected (cooldown period)
      // This gives the 751 time to restart BLE advertising
      bool in_cooldown = false;
      for (int i = 0; i < MAX_GSH_DEVICES; i++) {
        if (gsh_devices[i].state == GSH_STATE_DISCONNECTED &&
            gsh_devices[i].disconnect_time > 0) {
          // Cooldown: 2s base + 1s per consecutive failure (max 10s)
          uint32_t cooldown_ms =
              2000 + gsh_devices[i].connect_fail_count * 1000;
          if (cooldown_ms > 10000)
            cooldown_ms = 10000;
          if ((now - gsh_devices[i].disconnect_time) < cooldown_ms) {
            in_cooldown = true;
            break;
          }
        }
      }

      // Scan less frequently when devices are already connected
      uint32_t scan_gap = (gsh_get_connected_count() > 0) ? 10000 : 3000;
      if (!in_cooldown && (now - last_scan_check > scan_gap)) {
        last_scan_check = now;
        gsh_ble_start_scan();
      }
    }
  }
}

uint8_t gsh_get_connected_count() {
  uint8_t count = 0;
  for (int i = 0; i < MAX_GSH_DEVICES; i++) {
    if (gsh_devices[i].state >= GSH_STATE_AUTH_PENDING &&
        gsh_devices[i].state != GSH_STATE_ERROR) {
      count++;
    }
  }
  return count;
}

gsh_device_state_t gsh_get_device_state(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return GSH_STATE_DISCONNECTED;
  return gsh_devices[index].state;
}

const char *gsh_state_to_string(gsh_device_state_t state) {
  switch (state) {
  case GSH_STATE_DISCONNECTED:
    return "DISCONNECTED";
  case GSH_STATE_SCANNING:
    return "SCANNING";
  case GSH_STATE_CONNECTING:
    return "CONNECTING";
  case GSH_STATE_WAIT_FFE3_NOTIFY:
    return "WAIT_FFE3_NOTIFY";
  case GSH_STATE_AUTH_PENDING:
    return "AUTH_PENDING";
  case GSH_STATE_DATARATE_PENDING:
    return "DATARATE_PENDING";
  case GSH_STATE_WAIT_FFE1_NOTIFY:
    return "WAIT_FFE1_NOTIFY";
  case GSH_STATE_STREAMING:
    return "STREAMING";
  case GSH_STATE_ERROR:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

bool gsh_is_device_streaming(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return false;
  return gsh_devices[index].state == GSH_STATE_STREAMING;
}

uint32_t gsh_get_last_packet_time(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return 0;
  return gsh_devices[index].last_packet_time;
}

int8_t gsh_get_battery_level(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return -1;
  return gsh_devices[index].battery_level;
}

uint8_t gsh_get_charging_status(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return 0;
  return gsh_devices[index].charging_status;
}

int8_t gsh_get_rssi(uint8_t index) {
  if (index >= MAX_GSH_DEVICES)
    return 0;
  return gsh_devices[index].rssi;
}

bool gsh_get_mac_address(uint8_t index, uint8_t *out_mac) {
  if (index >= MAX_GSH_DEVICES || out_mac == nullptr)
    return false;
  if (gsh_devices[index].address == nullptr)
    return false;
  // BLEAddress stores native bytes (6 bytes)
  const uint8_t *native = *gsh_devices[index].address->getNative();
  // ESP32 BLE stores in reverse order; output as big-endian (MSB first)
  for (int i = 0; i < 6; i++) {
    out_mac[i] = native[5 - i];
  }
  return true;
}

// ============================================================================
// Private Helper Functions
// ============================================================================

static void changeState(gsh_device_t *device, gsh_device_state_t newState) {
  if (device->state != newState) {
    GSH_LOG("[GSH%d] State: %s -> %s\n", device->index,
            gsh_state_to_string(device->state), gsh_state_to_string(newState));
    device->state = newState;
    device->state_enter_time = millis();
  }
}

static uint16_t calculatePassword(BLEAddress *address) {
  // Password = sum of all MAC address bytes
  // Example: MAC 70:52:EA:E5:51:D1 -> 0x70+0x52+0xEA+0xE5+0x51+0xD1 = 0x03B3
  std::string addrStr = address->toString();
  uint16_t sum = 0;

  // Parse "XX:XX:XX:XX:XX:XX" format
  for (int i = 0; i < 6; i++) {
    int pos = i * 3; // Each byte takes 3 chars "XX:"
    char hex[3] = {addrStr[pos], addrStr[pos + 1], 0};
    sum += (uint8_t)strtol(hex, nullptr, 16);
  }

  return sum;
}

static bool sendPassword(gsh_device_t *device) {
  if (device->charFFE2 == nullptr)
    return false;

  // Device expects: CMD_PWD(0xCC) + password_H + password_L
  uint8_t cmd[3];
  cmd[0] = GSH_CMD_SET_PASSWORD;           // 0xCC
  cmd[1] = (device->password >> 8) & 0xFF; // High byte
  cmd[2] = device->password & 0xFF;        // Low byte

  GSH_LOG("[GSH%d] Sending password: 0x%02X 0x%02X 0x%02X\n", device->index,
          cmd[0], cmd[1], cmd[2]);

  device->charFFE2->writeValue(cmd, sizeof(cmd));
  return true;
}

static bool sendDataRate(gsh_device_t *device, uint8_t rate) {
  if (device->charFFE2 == nullptr)
    return false;

  if (device->fw_version == GSH_FW_V605) {
    // V605 firmware expects 3 bytes: {CMD, param1, param2}
    uint8_t cmd[3];
    cmd[0] = rate;
    switch (rate) {
    case GSH_CMD_256HZ_8BIT:
      cmd[1] = 0x80;
      cmd[2] = 0x08;
      break;
    case GSH_CMD_256HZ_16BIT:
      cmd[1] = 0x80;
      cmd[2] = 0x10;
      break;
    case GSH_CMD_512HZ_8BIT:
      cmd[1] = 0xFF;
      cmd[2] = 0x08;
      break;
    case GSH_CMD_512HZ_16BIT:
      cmd[1] = 0xFF;
      cmd[2] = 0x10;
      break;
    default:
      cmd[1] = 0x80;
      cmd[2] = 0x08;
      break;
    }
    GSH_LOG("[GSH%d] Setting data rate (V605): 0x%02X 0x%02X 0x%02X\n",
            device->index, cmd[0], cmd[1], cmd[2]);
    device->charFFE2->writeValue(cmd, sizeof(cmd));
  } else {
    // Legacy firmware expects 1 byte
    uint8_t cmd[1] = {rate};
    GSH_LOG("[GSH%d] Setting data rate (legacy): 0x%02X\n", device->index,
            cmd[0]);
    device->charFFE2->writeValue(cmd, sizeof(cmd));
  }
  return true;
}
