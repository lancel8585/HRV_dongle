#include "mac_whitelist.h"
#include <Preferences.h>

static Preferences prefs;
static uint8_t whitelist_macs[WHITELIST_MAX_SLOTS][6];

static const uint8_t WILDCARD_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const char *NVS_NAMESPACE = "whitelist";
static const char *NVS_KEYS[WHITELIST_MAX_SLOTS] = {"mac0", "mac1", "mac2",
                                                     "mac3"};

void whitelist_init() {
  prefs.begin(NVS_NAMESPACE, true); // read-only

  for (int i = 0; i < WHITELIST_MAX_SLOTS; i++) {
    size_t len = prefs.getBytes(NVS_KEYS[i], whitelist_macs[i], 6);
    if (len != 6) {
      // Not found or corrupted — default to wildcard
      memcpy(whitelist_macs[i], WILDCARD_MAC, 6);
    }
    Serial.printf("[WL] Slot %d: %02X:%02X:%02X:%02X:%02X:%02X\n", i,
                  whitelist_macs[i][0], whitelist_macs[i][1],
                  whitelist_macs[i][2], whitelist_macs[i][3],
                  whitelist_macs[i][4], whitelist_macs[i][5]);
  }

  prefs.end();
}

bool whitelist_set_mac(uint8_t slot, const uint8_t mac[6]) {
  if (slot >= WHITELIST_MAX_SLOTS)
    return false;

  memcpy(whitelist_macs[slot], mac, 6);

  prefs.begin(NVS_NAMESPACE, false); // read-write
  size_t written = prefs.putBytes(NVS_KEYS[slot], mac, 6);
  prefs.end();

  Serial.printf("[WL] Set slot %d: %02X:%02X:%02X:%02X:%02X:%02X (wrote %d)\n",
                slot, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], written);
  return written == 6;
}

bool whitelist_get_mac(uint8_t slot, uint8_t mac[6]) {
  if (slot >= WHITELIST_MAX_SLOTS)
    return false;

  memcpy(mac, whitelist_macs[slot], 6);
  return true;
}

// Build the byte-reversed version of a MAC address.
static void reverse_mac(const uint8_t in[6], uint8_t out[6]) {
  for (int i = 0; i < 6; i++)
    out[i] = in[5 - i];
}

bool whitelist_is_allowed(const uint8_t mac[6]) {
  uint8_t mac_rev[6];
  reverse_mac(mac, mac_rev);

  for (int i = 0; i < WHITELIST_MAX_SLOTS; i++) {
    // Wildcard slot allows anything
    if (memcmp(whitelist_macs[i], WILDCARD_MAC, 6) == 0)
      return true;
    // Match in either byte order (tolerates big-endian or little-endian stored MAC)
    if (memcmp(whitelist_macs[i], mac, 6) == 0)
      return true;
    if (memcmp(whitelist_macs[i], mac_rev, 6) == 0)
      return true;
  }
  return false;
}

bool whitelist_is_exact_match(const uint8_t mac[6]) {
  uint8_t mac_rev[6];
  reverse_mac(mac, mac_rev);

  for (int i = 0; i < WHITELIST_MAX_SLOTS; i++) {
    // Skip wildcard slots
    if (memcmp(whitelist_macs[i], WILDCARD_MAC, 6) == 0)
      continue;
    // Match in either byte order
    if (memcmp(whitelist_macs[i], mac, 6) == 0)
      return true;
    if (memcmp(whitelist_macs[i], mac_rev, 6) == 0)
      return true;
  }
  return false;
}

uint8_t whitelist_get_bound_count() {
  uint8_t count = 0;
  for (int i = 0; i < WHITELIST_MAX_SLOTS; i++) {
    if (memcmp(whitelist_macs[i], WILDCARD_MAC, 6) != 0)
      count++;
  }
  return count;
}

void whitelist_reset_all() {
  prefs.begin(NVS_NAMESPACE, false); // read-write
  for (int i = 0; i < WHITELIST_MAX_SLOTS; i++) {
    memcpy(whitelist_macs[i], WILDCARD_MAC, 6);
    prefs.putBytes(NVS_KEYS[i], WILDCARD_MAC, 6);
  }
  prefs.end();
  Serial.println("[WL] All slots reset to FF:FF:FF:FF:FF:FF");
}
