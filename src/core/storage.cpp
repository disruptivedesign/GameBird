#include "storage.h"
#include <Preferences.h>

// Each call opens the namespace, does its work, and closes it. High scores and
// settings are read once and written rarely, so the open/close cost is moot and
// we never hold NVS handles open across games.

void Storage::begin(){
  // NVS is initialized by the Arduino core; nothing required yet.
  // Reserved for future migration/versioning hooks.
}

uint32_t Storage::getU32(const char* ns, const char* key, uint32_t def){
  Preferences p;
  if (!p.begin(ns, true)) return def;   // read-only; false if ns doesn't exist yet
  uint32_t v = p.getUInt(key, def);
  p.end();
  return v;
}

void Storage::putU32(const char* ns, const char* key, uint32_t val){
  Preferences p;
  if (!p.begin(ns, false)) return;
  p.putUInt(key, val);
  p.end();
}

int32_t Storage::getI32(const char* ns, const char* key, int32_t def){
  Preferences p;
  if (!p.begin(ns, true)) return def;
  int32_t v = p.getInt(key, def);
  p.end();
  return v;
}

void Storage::putI32(const char* ns, const char* key, int32_t val){
  Preferences p;
  if (!p.begin(ns, false)) return;
  p.putInt(key, val);
  p.end();
}

void Storage::clearNamespace(const char* ns){
  Preferences p;
  if (!p.begin(ns, false)) return;
  p.clear();
  p.end();
}
