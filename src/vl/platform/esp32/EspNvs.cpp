#include "vl/platform/esp32/EspNvs.h"

#if VL_PLATFORM_ESP32

#include "nvs.h"
#include "nvs_flash.h"
#include "vl/Log.h"

namespace vl {

NvsStore::~NvsStore() {
  if (open_) nvs_close(handle_);
}

bool NvsStore::open() {
  if (open_) return true;
  esp_err_t err = nvs_flash_init();  // ESP_OK when the application already did it
  if (err != ESP_OK) {
    VL_LOGE("nvs: init failed: %s (the partition may need nvs_flash_erase())", esp_err_to_name(err));
    return false;
  }
  nvs_handle_t h = 0;
  err = nvs_open(ns_, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    VL_LOGE("nvs: open %s: %s", ns_, esp_err_to_name(err));
    return false;
  }
  handle_ = h;
  open_ = true;
  return true;
}

Result NvsStore::get(const char* key, void* buf, size_t cap, size_t& len) {
  if (!open()) return Result::NotReady;
  size_t n = cap;
  esp_err_t err = nvs_get_blob(handle_, key, buf, &n);
  if (err == ESP_ERR_NVS_NOT_FOUND) return Result::Rejected;
  if (err == ESP_ERR_NVS_INVALID_LENGTH) return Result::TooLarge;
  if (err != ESP_OK) {
    VL_LOGW("nvs: get %s: %s", key, esp_err_to_name(err));
    return Result::Rejected;
  }
  len = n;
  return Result::Ok;
}

Result NvsStore::set(const char* key, const void* value, size_t len) {
  if (!open()) return Result::NotReady;
  esp_err_t err = nvs_set_blob(handle_, key, value, len);
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    VL_LOGE("nvs: set %s: %s", key, esp_err_to_name(err));
    return err == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? Result::NoMemory : Result::Rejected;
  }
  return Result::Ok;
}

Result NvsStore::erase(const char* key) {
  if (!open()) return Result::NotReady;
  esp_err_t err = nvs_erase_key(handle_, key);
  if (err == ESP_ERR_NVS_NOT_FOUND) return Result::Ok;
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    VL_LOGE("nvs: erase %s: %s", key, esp_err_to_name(err));
    return Result::Rejected;
  }
  return Result::Ok;
}

}  // namespace vl

#endif  // VL_PLATFORM_ESP32
