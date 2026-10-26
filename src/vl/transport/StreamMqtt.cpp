#include "vl/transport/StreamMqtt.h"

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Mqtt.h"

namespace vl {

StreamMqtt::StreamMqtt(INetStream& stream, uint8_t* buf, size_t cap, IClock* clock)
    : stream_(stream), buf_(buf), cap_(cap), clock_(clock ? clock : &platformClock()) {}

StreamMqtt::~StreamMqtt() { disconnect(); }

Result StreamMqtt::validate(const ConnectOptions& o) const {
  if (o.tls && !stream_.secure()) {
    VL_LOGE("stream-mqtt: Config::tls is set but the stream is plain TCP");
    return Result::Rejected;
  }
  if (o.tls && o.tls->secureElement() && !stream_.secureElement()) {
    VL_LOGE("stream-mqtt: a secure element needs a stream that holds one (AteccTlsClient)");
    return Result::Rejected;
  }
  return Result::Ok;
}

uint16_t StreamMqtt::nextId() {
  if (++lastId_ == 0) lastId_ = 1;
  return lastId_;
}

bool StreamMqtt::send(const uint8_t* p, size_t n) {
  if (!stream_.write(p, n)) {
    close();
    return false;
  }
  lastTx_ = clock_->uptimeMs();
  return true;
}

void StreamMqtt::close() {
  if (state_ == Idle) return;
  stream_.stop();
  state_ = Idle;
  have_ = 0;
  skip_ = 0;
  ++gen_;
  closedPending_ = true;
}

void StreamMqtt::beginConnect(const ConnectOptions& o) {
  if (state_ != Idle) stream_.stop();
  state_ = Idle;
  ++gen_;
  have_ = 0;
  skip_ = 0;
  closedPending_ = false;
  memset(subs_, 0, sizeof(subs_));
  keepAliveMs_ = static_cast<uint32_t>(o.keepAliveS) * 1000u;
  uint8_t pkt[256];
  size_t n = mqtt::connect(pkt, sizeof(pkt), o.clientId, o.username, o.password, o.keepAliveS);
  if (n == 0 || !stream_.connect(o.host, o.port)) {
    closedPending_ = true;  // reported from loop(), never from here
    return;
  }
  state_ = AwaitConnack;
  lastRx_ = clock_->uptimeMs();
  if (!send(pkt, n)) return;
}

void StreamMqtt::disconnect() {
  if (state_ != Idle) {
    uint8_t pkt[2];
    stream_.write(pkt, mqtt::disconnect(pkt, sizeof(pkt)));
    stream_.stop();
  }
  state_ = Idle;
  ++gen_;
  have_ = 0;
  skip_ = 0;
  closedPending_ = false;
}

bool StreamMqtt::publish(const char* topic, const uint8_t* p, size_t n, uint8_t qos, uint16_t* packetId) {
  if (state_ != Online || qos > 1) return false;
  uint16_t id = qos ? nextId() : 0;
  uint8_t head[160];
  size_t h = mqtt::publishHead(head, sizeof(head), topic, n, qos, id);
  if (h == 0) return false;
  if (!send(head, h) || (n && !send(p, n))) return false;
  if (packetId) *packetId = id;
  return true;
}

void StreamMqtt::subscribe(const char* filter, uint8_t qos) {
  if (state_ != Online) return;
  Sub* slot = nullptr;
  for (size_t i = 0; i < VL_MAX_SUBSCRIPTIONS && !slot; ++i) {
    if (!subs_[i].filter) slot = &subs_[i];
  }
  uint8_t pkt[160];
  uint16_t id = nextId();
  size_t n = slot ? mqtt::subscribe(pkt, sizeof(pkt), id, filter, qos) : 0;
  if (n == 0) {
    if (subackCb_) subackCb_(filter, false, subackCtx_);
    return;
  }
  slot->id = id;
  slot->filter = filter;
  send(pkt, n);
}

void StreamMqtt::loop() {
  if (state_ != Idle) {
    uint64_t now = clock_->uptimeMs();
    if (!receive()) {
      close();
    } else if (state_ != Idle && keepAliveMs_) {
      now = clock_->uptimeMs();
      if (now - lastRx_ >= keepAliveMs_ + keepAliveMs_ / 2) {
        VL_LOGW("stream-mqtt: no traffic for 1.5 keep-alive periods, closing");
        close();
      } else if (state_ == Online && now - lastTx_ >= keepAliveMs_) {
        uint8_t pkt[2];
        send(pkt, mqtt::pingreq(pkt, sizeof(pkt)));
      }
    }
  }
  if (closedPending_) {
    closedPending_ = false;
    if (closedCb_) closedCb_(closedCtx_);
  }
}

// Reads what is waiting and dispatches every complete packet. False when the
// link failed or sent something malformed.
bool StreamMqtt::receive() {
  for (;;) {
    int n;
    if (skip_) {
      size_t want = skip_ < cap_ ? skip_ : cap_;
      n = stream_.read(buf_, want);
      if (n > 0) skip_ -= static_cast<size_t>(n);
    } else {
      if (have_ == cap_) return true;  // cannot happen: oversize packets are skipped
      n = stream_.read(buf_ + have_, cap_ - have_);
      if (n > 0) have_ += static_cast<size_t>(n);
    }
    if (n < 0) return false;
    if (n == 0) return true;
    lastRx_ = clock_->uptimeMs();
    if (skip_ || !have_) continue;

    size_t off = 0;
    uint32_t gen = gen_;
    while (off < have_) {
      mqtt::Header h;
      mqtt::Parse p = mqtt::header(buf_ + off, have_ - off, h);
      if (p == mqtt::Parse::Malformed) return false;
      if (p == mqtt::Parse::NeedMore) break;
      size_t total = h.size + h.remaining;
      if (total > cap_) {
        // Too large to hold: acknowledge a QoS 1 publish, then skip it.
        if (h.type == mqtt::kPublish && ((h.flags >> 1) & 3) == 1) {
          const uint8_t* b = buf_ + off + h.size;
          size_t got = have_ - off - h.size;
          if (got < 2 && h.size + 2 <= cap_) break;
          size_t idAt = got >= 2 ? 2 + ((b[0] << 8) | b[1]) : cap_;
          if (h.size + idAt + 2 <= cap_) {
            if (got < idAt + 2) break;
            uint8_t ack[4];
            if (!send(ack, mqtt::puback(ack, sizeof(ack), static_cast<uint16_t>((b[idAt] << 8) | b[idAt + 1])))) {
              return true;
            }
          }
        }
        ++dropped_;
        skip_ = total - (have_ - off);
        have_ = off;
        break;
      }
      if (have_ - off < total) break;
      dispatch(h.type, h.flags, buf_ + off + h.size, h.remaining);
      if (gen != gen_) return true;  // a callback reconnected or closed: the buffer is gone
      off += total;
    }
    memmove(buf_, buf_ + off, have_ - off);
    have_ -= off;
  }
}

void StreamMqtt::dispatch(uint8_t type, uint8_t flags, const uint8_t* body, size_t n) {
  switch (type) {
    case mqtt::kConnack: {
      uint8_t rc;
      if (state_ != AwaitConnack || !mqtt::connack(body, n, rc)) return;
      if (rc == 0) state_ = Online;
      if (connackCb_) connackCb_(rc, connackCtx_);
      return;
    }
    case mqtt::kPublish: {
      const uint8_t* topic;
      const uint8_t* payload;
      size_t topicLen, payloadLen;
      uint8_t qos;
      uint16_t id;
      if (!mqtt::publish(flags, body, n, topic, topicLen, qos, id, payload, payloadLen)) return;
      if (qos == 1) {
        uint8_t ack[4];
        if (!send(ack, mqtt::puback(ack, sizeof(ack), id))) return;
      }
      if (topicLen >= kTopicSize) {
        ++dropped_;
        return;
      }
      memcpy(topic_, topic, topicLen);
      topic_[topicLen] = 0;
      if (messageCb_) messageCb_(topic_, payload, payloadLen, messageCtx_);
      return;
    }
    case mqtt::kPuback: {
      uint16_t id;
      if (mqtt::ack(body, n, id) && pubackCb_) pubackCb_(id, pubackCtx_);
      return;
    }
    case mqtt::kSuback: {
      uint16_t id;
      bool granted;
      if (!mqtt::suback(body, n, id, granted)) return;
      for (size_t i = 0; i < VL_MAX_SUBSCRIPTIONS; ++i) {
        if (subs_[i].filter && subs_[i].id == id) {
          const char* f = subs_[i].filter;
          subs_[i].filter = nullptr;
          if (subackCb_) subackCb_(f, granted, subackCtx_);
          return;
        }
      }
      return;
    }
    default:
      return;  // PINGRESP, and anything a client never expects
  }
}

}  // namespace vl
