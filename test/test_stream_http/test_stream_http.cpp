// StreamHttp over scripted streams: URL parsing, the request it writes,
// Range resume, redirects, refused replies, body framing, and timeouts.
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/transport/StreamHttp.h"

using namespace vl;
using vltest::FakeClock;

void setUp() {}
void tearDown() {}

namespace {

// Each connect() serves the next scripted reply. A drained reply reads as a
// closed connection, unless hold is set (the server goes quiet instead).
struct HttpStream : INetStream {
  bool accept = true;
  bool hold = false;
  bool judge = false;
  Result verdict = Result::Ok;
  std::vector<std::string> replies;
  size_t next = 0;
  std::string in;
  std::string out;
  std::vector<std::string> hosts;
  std::vector<uint16_t> ports;
  const ITlsProvider* tls = nullptr;
  int connects = 0;
  int stops = 0;

  Result validate(const ITlsProvider* t) const override { return judge ? verdict : INetStream::validate(t); }
  bool connect(const char* h, uint16_t p, const ITlsProvider* t) override {
    ++connects;
    hosts.push_back(h);
    ports.push_back(p);
    tls = t;
    if (!accept) return false;
    in = next < replies.size() ? replies[next++] : std::string();
    return true;
  }
  int read(uint8_t* buf, size_t n) override {
    if (in.empty()) return hold ? 0 : -1;
    size_t k = n < in.size() ? n : in.size();
    memcpy(buf, in.data(), k);
    in.erase(0, k);
    return static_cast<int>(k);
  }
  bool write(const uint8_t* p, size_t n) override {
    out.append(reinterpret_cast<const char*>(p), n);
    return true;
  }
  void stop() override { ++stops; }
};

struct FakeTls : ITlsProvider {
  const char* caPem() const override { return "CA"; }
};

std::string ok200(const std::string& body) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string readAll(StreamHttp& http, int* last = nullptr) {
  std::string got;
  uint8_t buf[4];
  for (;;) {
    int n = http.read(buf, sizeof(buf));
    if (n <= 0) {
      if (last) *last = n;
      return got;
    }
    got.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
  }
}

const std::string kAgent = std::string("User-Agent: ViraLink/") + VL_SDK_VERSION + "\r\n";

}  // namespace

static void test_parse_url() {
  HttpUrl u;
  TEST_ASSERT_TRUE(parseHttpUrl("http://files.example.com/fw/a.bin?x=1", u));
  TEST_ASSERT_FALSE(u.tls);
  TEST_ASSERT_EQUAL_STRING("files.example.com", u.host);
  TEST_ASSERT_EQUAL_UINT16(80, u.port);
  TEST_ASSERT_EQUAL_STRING("/fw/a.bin?x=1", u.path);
  TEST_ASSERT_FALSE(u.explicitPort);

  TEST_ASSERT_TRUE(parseHttpUrl("HTTPS://h:8443", u));
  TEST_ASSERT_TRUE(u.tls);
  TEST_ASSERT_EQUAL_STRING("h", u.host);
  TEST_ASSERT_EQUAL_UINT16(8443, u.port);
  TEST_ASSERT_TRUE(u.explicitPort);
  TEST_ASSERT_EQUAL_STRING("/", u.path);

  TEST_ASSERT_TRUE(parseHttpUrl("https://10.0.0.5/f", u));
  TEST_ASSERT_EQUAL_UINT16(443, u.port);
  TEST_ASSERT_EQUAL_STRING("10.0.0.5", u.host);

  TEST_ASSERT_TRUE(parseHttpUrl("http://[fd00::1]:8080/f", u));
  TEST_ASSERT_EQUAL_STRING("fd00::1", u.host);
  TEST_ASSERT_EQUAL_UINT16(8080, u.port);

  const char* bad[] = {"ftp://h/f", "http://", "http:///f", "http://h:/f", "http://h:0/f", "http://h:65536/f",
                       "http://h:80x/f", "http://[fd00::1/f", "h/f", "http://user@h/f",
                       "http://h/f\r\nX-Evil: 1", "http://h/f HTTP/1.1", "http://h/\x7f", "http://h\tx/f",
                       "http://h%0d/f", "http://h;x/f", "http://h:x@y/f", "http://[fd00::1;x]/f", nullptr};
  for (int i = 0; bad[i]; ++i) TEST_ASSERT_FALSE_MESSAGE(parseHttpUrl(bad[i], u), bad[i]);

  std::string longHost = "http://" + std::string(sizeof(u.host), 'a') + "/f";
  TEST_ASSERT_FALSE(parseHttpUrl(longHost.c_str(), u));
}

static void test_get_writes_request_and_reads_body() {
  FakeClock clock;
  HttpStream net;
  net.replies.push_back(ok200("hello"));
  StreamHttp http(net, nullptr, &clock);
  size_t start = 99, total = 99;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://files.example.com/fw.bin", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_STRING("files.example.com", net.hosts[0].c_str());
  TEST_ASSERT_EQUAL_UINT16(80, net.ports[0]);
  TEST_ASSERT_NULL(net.tls);
  TEST_ASSERT_EQUAL_STRING(("GET /fw.bin HTTP/1.0\r\nHost: files.example.com\r\n" + kAgent + "\r\n").c_str(),
                           net.out.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, start);
  TEST_ASSERT_EQUAL_UINT32(5, total);
  int last = 1;
  TEST_ASSERT_EQUAL_STRING("hello", readAll(http, &last).c_str());
  TEST_ASSERT_EQUAL_INT(0, last);
  http.close();
  TEST_ASSERT_EQUAL_INT(1, net.stops);
  TEST_ASSERT_EQUAL_INT(-1, http.read(reinterpret_cast<uint8_t*>(&last), 1));
}

static void test_explicit_port_goes_in_host_header() {
  FakeClock clock;
  HttpStream net;
  net.replies.push_back(ok200("x"));
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h:8080/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_UINT16(8080, net.ports[0]);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, net.out.find("Host: h:8080\r\n"));

  HttpStream v6;
  v6.replies.push_back(ok200("x"));
  StreamHttp h6(v6, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, h6.open("http://[fd00::1]:8080/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_STRING("fd00::1", v6.hosts[0].c_str());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, v6.out.find("Host: [fd00::1]:8080\r\n"));
}

static void test_resume_with_range() {
  FakeClock clock;
  HttpStream net;
  net.replies.push_back("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 3-4/5\r\nContent-Length: 2\r\n\r\nlo");
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 3, nullptr, start, total));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, net.out.find("\r\nRange: bytes=3-\r\n"));
  TEST_ASSERT_EQUAL_UINT32(3, start);
  TEST_ASSERT_EQUAL_UINT32(5, total);
  TEST_ASSERT_EQUAL_STRING("lo", readAll(http).c_str());
}

static void test_range_ignored_restarts_at_zero() {
  FakeClock clock;
  HttpStream net;
  net.replies.push_back(ok200("hello"));
  StreamHttp http(net, nullptr, &clock);
  size_t start = 9, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 3, nullptr, start, total));
  TEST_ASSERT_EQUAL_UINT32(0, start);
  TEST_ASSERT_EQUAL_UINT32(5, total);
  TEST_ASSERT_EQUAL_STRING("hello", readAll(http).c_str());
}

static void test_redirects_are_followed() {
  FakeClock clock;
  HttpStream net, tlsNet;
  net.replies.push_back("HTTP/1.1 302 Found\r\nLocation: /moved/fw.bin\r\n\r\n");
  net.replies.push_back("HTTP/1.1 301 Moved\r\nlocation: https://cdn.example.com/fw.bin\r\n\r\n");
  tlsNet.judge = true;  // a TLS stream that accepts these trust anchors
  tlsNet.replies.push_back(ok200("abc"));
  FakeTls ca;
  StreamHttp http(net, &tlsNet, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h:81/fw.bin", 0, &ca, start, total));
  TEST_ASSERT_EQUAL_INT(2, net.connects);
  TEST_ASSERT_EQUAL_INT(2, net.stops);  // each redirect's connection is closed
  TEST_ASSERT_EQUAL_UINT16(81, net.ports[1]);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, net.out.find("GET /moved/fw.bin HTTP/1.0\r\nHost: h:81\r\n"));
  TEST_ASSERT_NULL(net.tls);
  TEST_ASSERT_EQUAL_STRING("cdn.example.com", tlsNet.hosts[0].c_str());
  TEST_ASSERT_EQUAL_UINT16(443, tlsNet.ports[0]);
  TEST_ASSERT_EQUAL_PTR(&ca, tlsNet.tls);
  TEST_ASSERT_EQUAL_STRING("abc", readAll(http).c_str());

  HttpStream other;  // scheme-relative: same scheme, another host
  other.replies.push_back("HTTP/1.1 302 Found\r\nLocation: //mirror.example.com/fw.bin\r\n\r\n");
  other.replies.push_back(ok200("m"));
  StreamHttp h2(other, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, h2.open("http://h/fw.bin", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_STRING("mirror.example.com", other.hosts[1].c_str());
  TEST_ASSERT_EQUAL_UINT16(80, other.ports[1]);
}

static void test_redirect_limits() {
  FakeClock clock;
  HttpStream net;
  for (int i = 0; i < 4; ++i) net.replies.push_back("HTTP/1.1 307 Again\r\nLocation: /f\r\n\r\n");
  net.replies.push_back(ok200("x"));
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_NOT_EQUAL(Result::Ok, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(4, net.connects);  // the first request and three redirects
  TEST_ASSERT_EQUAL_INT(4, net.stops);

  HttpStream bare;  // a redirect without a Location
  bare.replies.push_back("HTTP/1.1 302 Found\r\n\r\n");
  StreamHttp h2(bare, nullptr, &clock);
  TEST_ASSERT_NOT_EQUAL(Result::Ok, h2.open("http://h/f", 0, nullptr, start, total));

  HttpStream longLoc;  // a Location too long to keep is refused, not truncated
  longLoc.replies.push_back("HTTP/1.1 302 Found\r\nLocation: http://h/" + std::string(VL_OTA_URL_SIZE, 'p') +
                            "\r\n\r\n");
  StreamHttp h3(longLoc, nullptr, &clock);
  TEST_ASSERT_NOT_EQUAL(Result::Ok, h3.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(1, longLoc.connects);
}

static void test_https_never_redirects_to_http() {
  FakeClock clock;
  HttpStream net, tlsNet;
  tlsNet.judge = true;
  tlsNet.replies.push_back("HTTP/1.1 302 Found\r\nLocation: http://h/fw.bin\r\n\r\n");
  net.replies.push_back(ok200("plaintext"));
  StreamHttp http(net, &tlsNet, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Rejected, http.open("https://h/fw.bin", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(0, net.connects);
  TEST_ASSERT_EQUAL_INT(1, tlsNet.stops);

  // Nor through a scheme-relative or path-only Location after an https hop.
  HttpStream mixed, mixedTls;
  mixedTls.judge = true;
  mixed.replies.push_back("HTTP/1.1 302 Found\r\nLocation: https://h/a\r\n\r\n");
  mixedTls.replies.push_back("HTTP/1.1 302 Found\r\nLocation: http://h/b\r\n\r\n");
  StreamHttp h2(mixed, &mixedTls, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, h2.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(1, mixed.connects);
}

static void test_location_cannot_inject_headers() {
  FakeClock clock;
  HttpStream net;
  // A bare CR survives readLine(); the URL check stops it reaching the request.
  net.replies.push_back("HTTP/1.1 302 Found\r\nLocation: /f\rX-Evil: 1\r\n\r\n");
  net.replies.push_back(ok200("x"));
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Rejected, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(1, net.connects);
  TEST_ASSERT_EQUAL(std::string::npos, net.out.find("X-Evil"));
}

static void test_refused_replies() {
  FakeClock clock;
  size_t start, total;
  const char* replies[] = {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
                           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
                           "SSH-2.0-OpenSSH\r\n\r\n",
                           "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n",
                           "HTTP/1.1 200 OK\r\nContent-Length: 5",  // closed inside the headers
                           nullptr};
  for (int i = 0; replies[i]; ++i) {
    HttpStream net;
    net.replies.push_back(replies[i]);
    StreamHttp http(net, nullptr, &clock);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(Result::Ok, http.open("http://h/f", 0, nullptr, start, total), replies[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, net.stops, replies[i]);
    TEST_ASSERT_EQUAL_INT(-1, http.read(reinterpret_cast<uint8_t*>(&start), 1));
  }
}

static void test_https_needs_the_secure_stream() {
  FakeClock clock;
  size_t start, total;
  HttpStream net;
  StreamHttp plainOnly(net, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, plainOnly.open("https://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(0, net.connects);

  HttpStream tlsNet;
  tlsNet.judge = true;
  tlsNet.verdict = Result::Rejected;  // the secure stream cannot honour these trust anchors
  FakeTls ca;
  StreamHttp refusing(net, &tlsNet, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, refusing.open("https://h/f", 0, &ca, start, total));
  TEST_ASSERT_EQUAL_INT(0, tlsNet.connects);

  // A plain URL never hands the trust anchors to the plain stream.
  net.replies.push_back(ok200("x"));
  TEST_ASSERT_EQUAL(Result::Ok, plainOnly.open("http://h/f", 0, &ca, start, total));
  TEST_ASSERT_NULL(net.tls);
}

static void test_body_bounded_by_content_length() {
  FakeClock clock;
  HttpStream net;
  net.hold = true;  // a keep-alive server that does not close
  net.replies.push_back(ok200("hello") + "EXTRA");
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 0, nullptr, start, total));
  uint8_t buf[64];
  TEST_ASSERT_EQUAL_INT(5, http.read(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(0, http.read(buf, sizeof(buf)));
}

static void test_close_delimited_body() {
  FakeClock clock;
  clock.step = 100;
  HttpStream net;
  net.replies.push_back("HTTP/1.0 200 OK\r\nServer: tiny\r\n\r\nwhole file");
  StreamHttp http(net, nullptr, &clock);
  size_t start, total = 9;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_UINT32(0, total);
  uint8_t none;
  TEST_ASSERT_EQUAL_INT(0, http.read(&none, 0));  // nothing asked, nothing waited for
  int last = 1;
  TEST_ASSERT_EQUAL_STRING("whole file", readAll(http, &last).c_str());
  TEST_ASSERT_EQUAL_INT(0, last);
}

static void test_header_parsing_is_lenient() {
  FakeClock clock;
  HttpStream net;
  net.replies.push_back("HTTP/1.1 200 OK\r\nX-Junk: " + std::string(2000, 'j') +
                        "\r\ncontent-LENGTH:   3  \r\nX-Bare-LF: yes\n\r\nabc");
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_UINT32(3, total);
  TEST_ASSERT_EQUAL_STRING("abc", readAll(http).c_str());
}

static void test_header_timeout() {
  FakeClock clock;
  clock.step = 100;
  HttpStream net;
  net.hold = true;
  net.replies.push_back("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");  // the blank line never comes
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  uint64_t t0 = clock.now;
  TEST_ASSERT_EQUAL(Result::Timeout, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_TRUE(clock.now - t0 >= VL_OTA_HTTP_TIMEOUT_MS);
  TEST_ASSERT_TRUE(clock.now - t0 < VL_OTA_HTTP_TIMEOUT_MS + 1000);
  TEST_ASSERT_EQUAL_INT(1, net.stops);
}

static void test_read_failures() {
  FakeClock clock;
  clock.step = 100;
  size_t start, total;
  uint8_t buf[16];

  HttpStream quiet;  // the server stalls mid-body
  quiet.hold = true;
  quiet.replies.push_back("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
  StreamHttp stalled(quiet, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, stalled.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(3, stalled.read(buf, sizeof(buf)));
  uint64_t t0 = clock.now;
  TEST_ASSERT_EQUAL_INT(-1, stalled.read(buf, sizeof(buf)));
  TEST_ASSERT_TRUE(clock.now - t0 >= VL_OTA_HTTP_TIMEOUT_MS);

  HttpStream cut;  // the connection drops before Content-Length bytes arrived
  cut.replies.push_back("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
  StreamHttp broken(cut, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Ok, broken.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(3, broken.read(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(-1, broken.read(buf, sizeof(buf)));

  HttpStream down;
  down.accept = false;
  StreamHttp unreachable(down, nullptr, &clock);
  TEST_ASSERT_EQUAL(Result::Rejected, unreachable.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL(Result::Rejected, unreachable.open("not a url", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(1, down.connects);
}

static void test_open_again_closes_the_previous_download() {
  FakeClock clock;
  HttpStream net;
  net.hold = true;
  net.replies.push_back(ok200("first"));
  net.replies.push_back("HTTP/1.1 206 Partial\r\nContent-Length: 3\r\n\r\nond");
  StreamHttp http(net, nullptr, &clock);
  size_t start, total;
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 0, nullptr, start, total));
  TEST_ASSERT_EQUAL(Result::Ok, http.open("http://h/f", 3, nullptr, start, total));
  TEST_ASSERT_EQUAL_INT(1, net.stops);
  TEST_ASSERT_EQUAL_UINT32(6, total);
  uint8_t buf[16];
  TEST_ASSERT_EQUAL_INT(3, http.read(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_MEMORY("ond", buf, 3);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_url);
  RUN_TEST(test_get_writes_request_and_reads_body);
  RUN_TEST(test_explicit_port_goes_in_host_header);
  RUN_TEST(test_resume_with_range);
  RUN_TEST(test_range_ignored_restarts_at_zero);
  RUN_TEST(test_redirects_are_followed);
  RUN_TEST(test_redirect_limits);
  RUN_TEST(test_https_never_redirects_to_http);
  RUN_TEST(test_location_cannot_inject_headers);
  RUN_TEST(test_refused_replies);
  RUN_TEST(test_https_needs_the_secure_stream);
  RUN_TEST(test_body_bounded_by_content_length);
  RUN_TEST(test_close_delimited_body);
  RUN_TEST(test_header_parsing_is_lenient);
  RUN_TEST(test_header_timeout);
  RUN_TEST(test_read_failures);
  RUN_TEST(test_open_again_closes_the_previous_download);
  return UNITY_END();
}
