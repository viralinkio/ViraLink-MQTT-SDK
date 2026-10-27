#include "vl/transport/StreamHttp.h"

#include <stdio.h>
#include <string.h>

#include "vl/Log.h"
#include "vl/pal/Platform.h"

namespace vl {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool prefixNoCase(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix) {
    if (lower(*s) != *prefix) return false;
  }
  return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool endsAuthority(char c) { return c == '\0' || c == '/' || c == '?' || c == '#'; }

// Decimal digits and nothing else; false when empty or above max.
bool parseCount(const char* s, size_t max, size_t& out) {
  if (!isDigit(*s)) return false;
  size_t v = 0;
  for (; isDigit(*s); ++s) {
    size_t d = static_cast<size_t>(*s - '0');
    if (v > (max - d) / 10) return false;
    v = v * 10 + d;
  }
  if (*s) return false;
  out = v;
  return true;
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

}  // namespace

bool parseHttpUrl(const char* url, HttpUrl& out) {
  if (!url) return false;
  const char* p;
  if (prefixNoCase(url, "http://")) {
    out.tls = false;
    out.port = 80;
    p = url + 7;
  } else if (prefixNoCase(url, "https://")) {
    out.tls = true;
    out.port = 443;
    p = url + 8;
  } else {
    return false;
  }
  const char* host = p;
  const char* hostEnd;
  if (*p == '[') {  // IPv6 literal
    host = p + 1;
    hostEnd = strchr(host, ']');
    if (!hostEnd) return false;
    p = hostEnd + 1;
  } else {
    hostEnd = p + strcspn(p, ":/?#@");
    p = hostEnd;
  }
  size_t len = static_cast<size_t>(hostEnd - host);
  if (len == 0 || len >= sizeof(out.host)) return false;
  memcpy(out.host, host, len);
  out.host[len] = '\0';
  out.explicitPort = false;
  if (*p == ':') {
    ++p;
    uint32_t v = 0;
    const char* digits = p;
    for (; isDigit(*p); ++p) {
      v = v * 10 + static_cast<uint32_t>(*p - '0');
      if (v > 65535) return false;
    }
    if (p == digits || v == 0) return false;
    out.port = static_cast<uint16_t>(v);
    out.explicitPort = true;
  }
  if (!endsAuthority(*p)) return false;  // user info, or junk after the port
  out.path = *p == '/' ? p : "/";
  return true;
}

StreamHttp::StreamHttp(INetStream& plain, INetStream* secure, IClock* clock)
    : plain_(plain), secure_(secure), clock_(clock ? clock : &platformClock()) {}

Result StreamHttp::open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) {
  close();
  size_t len = url ? strlen(url) : sizeof(url_);
  if (len >= sizeof(url_)) return Result::Rejected;
  memcpy(url_, url, len + 1);
  for (int hop = 0;; ++hop) {
    HttpUrl u;
    if (!parseHttpUrl(url_, u)) {
      VL_LOGW("ota: not an http(s) URL");
      return Result::Rejected;
    }
    int status = 0;
    bool relocated = false;
    Result r = request(u, offset, tls);
    if (r == Result::Ok) r = readHeaders(u, status, relocated);
    if (r != Result::Ok) {
      stopStream();
      return r;
    }
    if (status == 200 || status == 206) {
      start = status == 206 ? offset : 0;
      total = sized_ ? start + length_ : 0;
      remaining_ = length_;
      return Result::Ok;
    }
    stopStream();
    if (!isRedirect(status) || hop == kMaxRedirects || !relocated) {
      VL_LOGW("ota: HTTP status %d", status);
      return Result::Rejected;
    }
  }
}

Result StreamHttp::request(const HttpUrl& u, size_t offset, const ITlsProvider* tls) {
  INetStream* s = &plain_;
  const ITlsProvider* t = nullptr;
  if (u.tls) {
    if (!secure_) {
      VL_LOGW("ota: an https:// URL needs StreamHttp's secure stream");
      return Result::Rejected;
    }
    s = secure_;
    t = tls;
    if (s->validate(t) != Result::Ok) return Result::Rejected;
  }
  stream_ = s;
  if (!s->connect(u.host, u.port, t)) {
    VL_LOGW("ota: cannot connect to %s:%u", u.host, static_cast<unsigned>(u.port));
    return Result::Rejected;
  }
  char num[24];
  bool ok = sendStr("GET ") && sendStr(u.path) && sendStr(" HTTP/1.0\r\nHost: ");
  bool v6 = strchr(u.host, ':') != nullptr;
  ok = ok && (!v6 || sendStr("[")) && sendStr(u.host) && (!v6 || sendStr("]"));
  if (ok && u.explicitPort) {
    snprintf(num, sizeof(num), ":%u", static_cast<unsigned>(u.port));
    ok = sendStr(num);
  }
  ok = ok && sendStr("\r\n");
  if (ok && offset) {
    snprintf(num, sizeof(num), "%lu-\r\n", static_cast<unsigned long>(offset));
    ok = sendStr("Range: bytes=") && sendStr(num);
  }
  ok = ok && sendStr("User-Agent: ViraLink/" VL_SDK_VERSION "\r\n\r\n");
  return ok ? Result::Ok : Result::Rejected;
}

Result StreamHttp::readHeaders(const HttpUrl& u, int& status, bool& relocated) {
  deadline_ = clock_->uptimeMs() + VL_OTA_HTTP_TIMEOUT_MS;
  timedOut_ = false;
  sized_ = false;
  length_ = 0;
  bool truncated;
  int n = readLine(truncated);
  if (n < 0) return timedOut_ ? Result::Timeout : Result::Rejected;
  // "HTTP/1.x SSS reason"
  if (n < 12 || !prefixNoCase(line_, "http/1.") || line_[8] != ' ' || !isDigit(line_[9]) || !isDigit(line_[10]) ||
      !isDigit(line_[11]) || (line_[12] != ' ' && line_[12] != '\0')) {
    VL_LOGW("ota: not an HTTP reply");
    return Result::Rejected;
  }
  status = (line_[9] - '0') * 100 + (line_[10] - '0') * 10 + (line_[11] - '0');
  bool chunked = false;
  for (;;) {
    n = readLine(truncated);
    if (n < 0) return timedOut_ ? Result::Timeout : Result::Rejected;
    if (n == 0) break;
    const char* v;
    if ((v = headerValue("content-length"))) {
      if (!parseCount(v, static_cast<size_t>(-1), length_)) {
        VL_LOGW("ota: bad Content-Length");
        return Result::Rejected;
      }
      sized_ = true;
    } else if ((v = headerValue("transfer-encoding"))) {
      chunked = !prefixNoCase(v, "identity");
    } else if ((v = headerValue("location"))) {
      relocated = !truncated && relocate(u, v);
    }
  }
  if (chunked) {
    VL_LOGW("ota: chunked replies are not supported");
    return Result::Rejected;
  }
  return Result::Ok;
}

const char* StreamHttp::headerValue(const char* name) {
  if (!prefixNoCase(line_, name)) return nullptr;
  char* v = line_ + strlen(name);
  if (*v != ':') return nullptr;
  ++v;
  while (*v == ' ' || *v == '\t') ++v;
  char* end = v + strlen(v);
  while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  return v;
}

bool StreamHttp::relocate(const HttpUrl& u, const char* loc) {
  int n;
  if (prefixNoCase(loc, "http://") || prefixNoCase(loc, "https://")) {
    n = snprintf(url_, sizeof(url_), "%s", loc);
  } else if (loc[0] == '/' && loc[1] == '/') {  // same scheme, another host
    n = snprintf(url_, sizeof(url_), "%s:%s", u.tls ? "https" : "http", loc);
  } else if (loc[0] == '/') {
    bool v6 = strchr(u.host, ':') != nullptr;
    n = snprintf(url_, sizeof(url_), "%s://%s%s%s:%u%s", u.tls ? "https" : "http", v6 ? "[" : "", u.host,
                 v6 ? "]" : "", static_cast<unsigned>(u.port), loc);
  } else {
    return false;  // relative to the current path: not used by file servers
  }
  return n > 0 && static_cast<size_t>(n) < sizeof(url_);
}

int StreamHttp::readLine(bool& truncated) {
  size_t len = 0;
  truncated = false;
  for (;;) {
    uint8_t c;
    int r = stream_->read(&c, 1);
    if (r < 0) return -1;
    if (r == 0) {
      if (clock_->uptimeMs() >= deadline_) {
        timedOut_ = true;
        VL_LOGW("ota: no HTTP reply");
        return -1;
      }
      platformYield();
      continue;
    }
    if (c == '\n') {
      if (len && line_[len - 1] == '\r') --len;
      line_[len] = '\0';
      return static_cast<int>(len);
    }
    if (len < sizeof(line_) - 1) {
      line_[len++] = static_cast<char>(c);
    } else {
      truncated = true;
    }
  }
}

int StreamHttp::read(uint8_t* buf, size_t cap) {
  if (!stream_) return -1;
  if (sized_) {
    if (!remaining_) return 0;
    if (cap > remaining_) cap = remaining_;
  }
  if (!cap) return 0;
  uint64_t deadline = clock_->uptimeMs() + VL_OTA_HTTP_TIMEOUT_MS;
  for (;;) {
    int n = stream_->read(buf, cap);
    if (n > 0) {
      if (sized_) remaining_ -= static_cast<size_t>(n);
      return n;
    }
    if (n < 0) return sized_ ? -1 : 0;  // without a length, the close ends the body
    if (clock_->uptimeMs() >= deadline) {
      VL_LOGW("ota: download stalled");
      return -1;
    }
    platformYield();
  }
}

void StreamHttp::close() { stopStream(); }

bool StreamHttp::sendStr(const char* s) { return stream_->write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

void StreamHttp::stopStream() {
  if (!stream_) return;
  stream_->stop();
  stream_ = nullptr;
}

}  // namespace vl
