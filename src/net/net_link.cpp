#include "net_link.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

NetLink*      NetLink::_self  = nullptr;
QueueHandle_t NetLink::_queue = nullptr;

// Runs in the WiFi task context: copy-to-queue only, never block or do work.
void NetLink::onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len){
  if (!_queue || len <= 0 || len > NET_MAX_PAYLOAD) return;
  RxFrame f;
  memcpy(f.src, info->src_addr, 6);
  f.len = (uint8_t)len;
  memcpy(f.data, data, (size_t)len);
  xQueueSend(_queue, &f, 0);   // 0 timeout: drop if the queue is full
}

bool NetLink::begin(uint8_t channel){
  _self = this;
  if (!_queue) _queue = xQueueCreate(12, sizeof(RxFrame));
  if (!_queue) return false;

  // Station mode but never associated to an AP -- ESP-NOW rides the radio
  // directly. We pin the channel ourselves since there is no AP to inherit it
  // from. (Promiscuous toggle is the reliable way to force it while idle.)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);        // modem sleep adds latency/jitter to ESP-NOW
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  WiFi.macAddress(_mac);

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(NetLink::onRecv);

  addPeer(NET_BROADCAST);
  return true;
}

void NetLink::addPeer(const uint8_t* mac){
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;               // 0 = current channel
  p.ifidx   = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

bool NetLink::unicast(const uint8_t* mac, const uint8_t* buf, size_t len){
  return esp_now_send(mac, buf, len) == ESP_OK;
}

bool NetLink::broadcast(const uint8_t* buf, size_t len){
  return esp_now_send(NET_BROADCAST, buf, len) == ESP_OK;
}

bool NetLink::poll(RxFrame& out){
  if (!_queue) return false;
  return xQueueReceive(_queue, &out, 0) == pdTRUE;
}
