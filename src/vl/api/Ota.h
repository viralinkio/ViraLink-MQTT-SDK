#pragma once

#include "vl/Config.h"

#if VL_ENABLE_OTA

#include <stddef.h>
#include <stdint.h>

#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/core/Hash.h"
#include "vl/core/JsonReader.h"
#include "vl/core/JsonWriter.h"
#include "vl/pal/Firmware.h"

namespace vl {

// fw_state values in the order an update reaches them; Idle is never reported.
enum class OtaState : uint8_t { Idle, Downloading, Downloaded, Verified, Updating, Updated, Failed };
const char* toString(OtaState s);  // "DOWNLOADING", ...

// error is set for Failed and null otherwise; valid during the call.
typedef void (*OtaStateCb)(OtaState state, const char* error, void* ctx);
// total is 0 when the size is unknown (an fw_url package without fw_size).
typedef void (*OtaProgressCb)(size_t done, size_t total, void* ctx);

// Firmware OTA over the ThingsBoard v2 firmware protocol.
//  - On the first Online after boot: reports current_fw_title/_version and
//    confirms a new image that is waiting for it (ESP32 rollback).
//  - On every Online, and on each fw_* shared-attribute update: reads the
//    assigned package. A package equal to the running firmware is reported
//    UPDATED (once per boot); any other is downloaded over MQTT chunks, or
//    over HTTP(S) when it has fw_url, hashed while it is written, verified,
//    activated, and the device restarts.
//  - A failed package is not tried again until it is unassigned and assigned
//    again, another one is assigned, or the device restarts.
// Runs on the client task; register callbacks before client.begin().
class Ota : public ApiModule {
 public:
  Ota();

  // The running firmware (copied). Required: until it is set the module does
  // nothing. Call before client.begin().
  Result begin(const char* title, const char* version);
  // Defaults: platformFirmwareWriter() and platformHttpSource().
  void setWriter(IFirmwareWriter& w) { writer_ = &w; }
  void setHttp(IHttpSource& h) { http_ = &h; }
  // Trust anchors for fw_url downloads; default: the client's Config::tls.
  void setHttpTls(const ITlsProvider* tls) { httpTls_ = tls; }
  void onState(OtaStateCb cb, void* ctx) {
    stateCb_ = cb;
    stateCtx_ = ctx;
  }
  void onProgress(OtaProgressCb cb, void* ctx) {
    progressCb_ = cb;
    progressCtx_ = ctx;
  }
  // Last reported state.
  OtaState state() const { return state_; }

  void topics(TopicSink& sink) override;
  bool handle(const Topic& topic, Bytes payload) override;
  void onOnline() override;
  void tick(uint64_t nowMs) override;
  void onDetach() override;

 private:
  enum class Phase : uint8_t { Idle, Downloading, Updating, Updated, Failed };

  struct Target {
    char title[VL_OTA_TITLE_SIZE];
    char version[VL_OTA_VERSION_SIZE];
    char checksum[Hasher::kHexSize];
    char url[VL_OTA_URL_SIZE];
    char algoName[16];
    size_t size;
  };

  void bootCheck(uint64_t nowMs);
  void requestTarget(uint64_t nowMs);
  static void onTargetResponse(Result r, const uint8_t* p, size_t n, void* ctx);
  // full: attrs is the whole assignment (a response), so a missing fw_title
  // means nothing is assigned. Otherwise it is an update notice.
  bool applyAttributes(JsonValue attrs, bool full);
  void applyTarget(const Target& t, const char* error);
  void unassign();
  void startDownload();
  void requestChunk(uint64_t nowMs);
  void onChunk(const Topic& topic, Bytes payload);
  void httpStep(uint64_t nowMs);
  void httpRetry(uint64_t nowMs, const char* error);
  bool consume(const uint8_t* p, size_t n);
  void finishDownload();
  void abortDownload();
  void fail(const char* error);
  bool sendTelemetry(const JsonWriter& w);
  void report(OtaState s, const char* error = nullptr);

  IFirmwareWriter* writer_;
  IHttpSource* http_;
  const ITlsProvider* httpTls_ = nullptr;
  OtaStateCb stateCb_ = nullptr;
  void* stateCtx_ = nullptr;
  OtaProgressCb progressCb_ = nullptr;
  void* progressCtx_ = nullptr;

  char title_[VL_OTA_TITLE_SIZE] = {};
  char version_[VL_OTA_VERSION_SIZE] = {};
  Target target_;
  Hasher hasher_;
  HashAlgo algo_ = HashAlgo::Md5;
  bool verify_ = false;  // target_ has a checksum

  OtaState state_ = OtaState::Idle;
  Phase phase_ = Phase::Idle;
  bool begun_ = false;
  bool booted_ = false;          // bootCheck() ran
  bool pendingConfirm_ = false;  // new image waiting for its first Online
  bool rolledBack_ = false;      // the next differing package fails as "rolled back"
  bool infoSent_ = false;
  bool updatedSent_ = false;
  bool writerOpen_ = false;
  bool httpOpen_ = false;
  bool restarting_ = false;
  uint64_t confirmDeadline_ = 0;
  uint64_t restartAt_ = 0;
  uint32_t targetReq_ = 0;  // pending attribute request, 0 = none
  bool needTarget_ = false;
  uint64_t targetRetryAt_ = 0;

  size_t written_ = 0;
  size_t expected_ = 0;   // image size, 0 = unknown until the download ends
  size_t chunkSize_ = 0;
  uint32_t chunk_ = 0;
  uint32_t firstReq_ = 1;  // chunk responses from earlier downloads are ignored
  uint32_t nextReq_ = 1;
  uint64_t deadline_ = 0;  // chunk timeout or HTTP retry time, 0 = none
  uint8_t retries_ = 0;
};

}  // namespace vl

#endif  // VL_ENABLE_OTA
