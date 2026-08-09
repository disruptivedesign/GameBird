#pragma once
#include <Arduino.h>

// ============================================================================
//  Storage: centralized persistence over ESP32 NVS. Games own their keys and
//  namespace (what to persist); this owns how it's stored, so system settings,
//  resets, and future versioning have a single home.
// ============================================================================

// Reserved namespace for future system-wide settings (brightness, volume,
// last-played game, ...). Nothing is stored here yet -- it's stubbed so the
// top-level menu has a home to persist to later.
#define STORAGE_NS_SYSTEM  "system"

class Storage {
public:
  void begin();

  uint32_t getU32(const char* ns, const char* key, uint32_t def);
  void     putU32(const char* ns, const char* key, uint32_t val);
  int32_t  getI32(const char* ns, const char* key, int32_t def);
  void     putI32(const char* ns, const char* key, int32_t val);

  void     clearNamespace(const char* ns);   // wipe one game's saved data
};
