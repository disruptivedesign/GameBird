#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_now.h>
#include "net_proto.h"

// ============================================================================
//  Layer 1 -- Transport. The only place that touches ESP-NOW. Wraps radio init,
//  channel selection, unicast/broadcast send, and a receive queue.
//
//  The ESP-NOW receive callback runs in the WiFi task, so it does nothing but
//  copy the frame into a FreeRTOS queue and return; the main loop drains it via
//  poll(). Doing work in the callback is the classic ESP-NOW footgun.
// ============================================================================

struct RxFrame {
  uint8_t src[6];
  uint8_t len;
  uint8_t data[NET_MAX_PAYLOAD];
};

class NetLink {
public:
  bool begin(uint8_t channel);
  void addPeer(const uint8_t* mac);                                  // idempotent
  bool unicast(const uint8_t* mac, const uint8_t* buf, size_t len);
  bool broadcast(const uint8_t* buf, size_t len);
  bool poll(RxFrame& out);                    // drain one frame; false if empty
  const uint8_t* mac() const { return _mac; } // this device's STA MAC

private:
  uint8_t _mac[6] = {0};
  static NetLink*     _self;
  static QueueHandle_t _queue;
  static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
};
