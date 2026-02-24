#ifndef MAC_WHITELIST_H
#define MAC_WHITELIST_H

#include <Arduino.h>

#define WHITELIST_MAX_SLOTS 4

// Initialize whitelist from NVS (call before gsh_ble_init)
void whitelist_init();

// Set MAC for a slot (0~3), writes to NVS. Returns true on success.
bool whitelist_set_mac(uint8_t slot, const uint8_t mac[6]);

// Get MAC for a slot (0~3). Returns true on success.
bool whitelist_get_mac(uint8_t slot, uint8_t mac[6]);

// Check if a MAC address is allowed by the whitelist.
// Returns true if mac matches any slot or if that slot is FF:FF:FF:FF:FF:FF (wildcard).
bool whitelist_is_allowed(const uint8_t mac[6]);

// Reset all slots to FF:FF:FF:FF:FF:FF (wildcard) and write to NVS.
void whitelist_reset_all();

#endif // MAC_WHITELIST_H
