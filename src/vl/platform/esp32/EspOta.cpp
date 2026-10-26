#include "vl/platform/esp32/EspOta.h"

#if VL_PLATFORM_ESP32

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "vl/Log.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
// See EspMqtt.cpp: arduino-esp32 shadows IDF's esp_crt_bundle.h.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
#define VL_HTTP_CRT_BUNDLE 1
#else
#define VL_HTTP_CRT_BUNDLE 0
#endif

namespace vl {

IFirmwareWriter* platformFirmwareWriter() {
  static EspFirmwareWriter writer;
  return &writer;
}

IHttpSource* platformHttpSource() {
  static EspHttpSource source;
  return &source;
}

Result EspFirmwareWriter::begin(size_t size) {
  abort();
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  if (!p) {
    VL_LOGE("ota: no OTA app partition (check the partition table)");
    return Result::NotReady;
  }
  if (size > p->size) return Result::TooLarge;
  // Erasing the whole slot up front would stall the client task for seconds.
  esp_ota_handle_t h = 0;
  esp_err_t err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (err != ESP_OK) {
    VL_LOGE("ota: esp_ota_begin: %s", esp_err_to_name(err));
    return Result::Rejected;
  }
  part_ = p;
  handle_ = h;
  open_ = true;
  return Result::Ok;
}

Result EspFirmwareWriter::write(const uint8_t* p, size_t n) {
  if (!open_) return Result::NotReady;
  esp_err_t err = esp_ota_write(handle_, p, n);
  if (err != ESP_OK) {
    VL_LOGE("ota: esp_ota_write: %s", esp_err_to_name(err));
    return Result::Rejected;
  }
  return Result::Ok;
}

Result EspFirmwareWriter::finish() {
  if (!open_) return Result::NotReady;
  open_ = false;
  esp_err_t err = esp_ota_end(handle_);  // checks the image header and digest
  if (err == ESP_OK) err = esp_ota_set_boot_partition(static_cast<const esp_partition_t*>(part_));
  if (err != ESP_OK) {
    VL_LOGE("ota: activation failed: %s", esp_err_to_name(err));
    return Result::Rejected;
  }
  return Result::Ok;
}

void EspFirmwareWriter::abort() {
  if (!open_) return;
  open_ = false;
  esp_ota_abort(handle_);
}

void EspFirmwareWriter::restart() { esp_restart(); }

bool EspFirmwareWriter::pendingConfirm() {
  esp_ota_img_states_t st;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
         st == ESP_OTA_IMG_PENDING_VERIFY;
}

void EspFirmwareWriter::confirm() { esp_ota_mark_app_valid_cancel_rollback(); }

void EspFirmwareWriter::rollback() { esp_ota_mark_app_invalid_rollback_and_reboot(); }

bool EspFirmwareWriter::rolledBack() { return esp_ota_get_last_invalid_partition() != nullptr; }

Result EspHttpSource::open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) {
  close();
  esp_http_client_config_t c;
  memset(&c, 0, sizeof(c));
  c.url = url;
  c.timeout_ms = VL_OTA_HTTP_TIMEOUT_MS;
  c.cert_pem = tls ? tls->caPem() : nullptr;
#if VL_HTTP_CRT_BUNDLE
  if (!c.cert_pem) c.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) return Result::NoMemory;
  client_ = h;
  if (offset) {
    char range[32];
    snprintf(range, sizeof(range), "bytes=%lu-", static_cast<unsigned long>(offset));
    esp_http_client_set_header(h, "Range", range);
  }
  long long len = -1;
  int status = 0;
  for (int hop = 0;; ++hop) {
    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
      VL_LOGW("ota: HTTP open failed: %s", esp_err_to_name(err));
      close();
      return Result::Rejected;
    }
    len = esp_http_client_fetch_headers(h);
    status = esp_http_client_get_status_code(h);
    bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!redirect || hop == 3) break;
    esp_http_client_set_redirection(h);
    esp_http_client_close(h);
  }
  if (status != 200 && status != 206) {
    VL_LOGW("ota: HTTP status %d", status);
    close();
    return Result::Rejected;
  }
  start = status == 206 ? offset : 0;
  total = len > 0 ? start + static_cast<size_t>(len) : 0;
  return Result::Ok;
}

int EspHttpSource::read(uint8_t* buf, size_t cap) {
  esp_http_client_handle_t h = static_cast<esp_http_client_handle_t>(client_);
  if (!h) return -1;
  int n = esp_http_client_read(h, reinterpret_cast<char*>(buf), static_cast<int>(cap));
  if (n > 0) return n;
  return esp_http_client_is_complete_data_received(h) ? 0 : -1;
}

void EspHttpSource::close() {
  esp_http_client_handle_t h = static_cast<esp_http_client_handle_t>(client_);
  if (!h) return;
  client_ = nullptr;
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
