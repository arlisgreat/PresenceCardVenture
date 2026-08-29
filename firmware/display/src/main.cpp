#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <time.h>

#include "DisplayBoard.h"
#include "device_config_stamp.h"

#if __has_include("device_config.h")
#include "device_config.h"
#else
#define PVC_WIFI_SSID ""
#define PVC_WIFI_PASSWORD ""
#define PVC_API_BASE_URL ""
#define PVC_TLS_ROOT_CA_PEM ""
#define PVC_ALLOW_HTTP_LOCAL_DEV 0
#endif

namespace {

constexpr char kFirmwareVersion[] = "0.1.0-display";
constexpr char kHardwareName[] = "esp32-8048S043C";
constexpr uint32_t kCloudPollMs = 10000;
constexpr uint32_t kPairPollMs = 3000;
constexpr uint32_t kPairRetryMs = 10000;
constexpr uint32_t kWifiRetryMs = 10000;
constexpr size_t kMaxJsonBytes = 16 * 1024;
constexpr size_t kMaxJpegBytes = 160 * 1024;
// ESP.getPsramSize() can report a few KiB below the nominal 8 MiB after
// initialization. Seven MiB still distinguishes the required 8 MiB-PSRAM board.
constexpr size_t kMinPsramBytes = 7 * 1024 * 1024;
constexpr uint16_t kExpectedJpegWidth = 320;
constexpr uint16_t kExpectedJpegHeight = 240;

constexpr uint32_t kBackground = 0xFBF9F7;
constexpr uint32_t kInk = 0x242529;
constexpr uint32_t kMuted = 0x74777D;
constexpr uint32_t kPink = 0xF7DCE5;
constexpr uint32_t kBlue = 0xDCE9F5;
constexpr uint32_t kSage = 0xDDE8DE;

PresenceDisplay display;
Preferences preferences;

String deviceId;
String apiBase;
String token;
String latestPhotoId;
String pairCode;
String statusText = "BOOTING";
String statusDetail;
bool configReady = false;
bool displayReady = false;
bool psramReady = false;
bool hasRenderedPhoto = false;
uint32_t nextPairActionAt = 0;
uint32_t nextCloudPollAt = 0;
uint32_t nextWifiAttemptAt = 0;
uint32_t nextTimeAttemptAt = 0;

struct HttpResponse {
  int code = -1;
  String body;
};

struct JpegDownload {
  int httpCode = -1;
  uint8_t* data = nullptr;
  size_t size = 0;
};

class BoundedWriteStream : public Stream {
 public:
  BoundedWriteStream(uint8_t* destination, size_t capacity)
      : destination_(destination), capacity_(capacity) {}

  size_t write(uint8_t value) override {
    return write(&value, 1);
  }

  size_t write(const uint8_t* source, size_t length) override {
    if (source == nullptr || length == 0) return 0;
    const size_t remaining = capacity_ - size_;
    const size_t accepted = length < remaining ? length : remaining;
    if (accepted > 0) {
      memcpy(destination_ + size_, source, accepted);
      size_ += accepted;
    }
    if (accepted != length) overflowed_ = true;
    return accepted;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t size() const { return size_; }
  bool overflowed() const { return overflowed_; }

 private:
  uint8_t* destination_;
  size_t capacity_;
  size_t size_ = 0;
  bool overflowed_ = false;
};

bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

String normalizedBaseUrl() {
  String value(PVC_API_BASE_URL);
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

String urlOrigin(const String& url) {
  const int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return String();
  const int pathStart = url.indexOf('/', schemeEnd + 3);
  return pathStart < 0 ? url : url.substring(0, pathStart);
}

String urlHost(const String& url) {
  const int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return String();
  int hostEnd = url.indexOf('/', schemeEnd + 3);
  if (hostEnd < 0) hostEnd = url.length();
  String authority = url.substring(schemeEnd + 3, hostEnd);
  const int at = authority.lastIndexOf('@');
  if (at >= 0) authority = authority.substring(at + 1);
  const int colon = authority.lastIndexOf(':');
  if (colon >= 0) authority = authority.substring(0, colon);
  authority.toLowerCase();
  return authority;
}

bool isPrivateIpv4(const String& host) {
  int a = -1, b = -1, c = -1, d = -1;
  char trailing = 0;
  if (sscanf(host.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &trailing) != 4) {
    return false;
  }
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) {
    return false;
  }
  return a == 10 || a == 127 || (a == 192 && b == 168) ||
         (a == 172 && b >= 16 && b <= 31);
}

bool isLocalHost(const String& host) {
  return host == "localhost" || host.endsWith(".local") || isPrivateIpv4(host);
}

bool urlAllowed(const String& url) {
  const int schemeEnd = url.indexOf("://");
  const int authorityEnd = url.indexOf('/', schemeEnd + 3);
  const int authorityLimit = authorityEnd < 0 ? url.length() : authorityEnd;
  const int credentialsMarker = url.indexOf('@', schemeEnd + 3);
  if (schemeEnd < 0 ||
      (credentialsMarker >= 0 && credentialsMarker < authorityLimit) ||
      url.indexOf('#') >= 0) {
    return false;
  }
  if (url.startsWith("https://")) {
    return strlen(PVC_TLS_ROOT_CA_PEM) > 0;
  }
  if (url.startsWith("http://")) {
    return PVC_ALLOW_HTTP_LOCAL_DEV == 1 && isLocalHost(urlHost(url));
  }
  return false;
}

bool sameOrigin(const String& left, const String& right) {
  String a = urlOrigin(left);
  String b = urlOrigin(right);
  a.toLowerCase();
  b.toLowerCase();
  return a == b;
}

String endpointUrl(const String& path) {
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  if (path.startsWith("/")) return apiBase + path;
  return apiBase + "/" + path;
}

String imageUrl(const String& value) {
  String resolved;
  if (value.startsWith("http://") || value.startsWith("https://")) {
    resolved = value;
  } else if (value.startsWith("/")) {
    resolved = urlOrigin(apiBase) + value;
  } else {
    resolved = apiBase + "/" + value;
  }
  resolved += resolved.indexOf('?') >= 0 ? "&size=320" : "?size=320";
  return resolved;
}

void drawChrome() {
  display.startWrite();
  display.fillRect(0, 0, 800, 82, kBackground);
  display.fillRect(0, 420, 800, 60, kBackground);
  display.fillRect(0, 80, 800, 2, kPink);
  display.fillRect(0, 420, 800, 2, kBlue);

  display.setTextColor(kInk, kBackground);
  display.setTextDatum(lgfx::textdatum_t::top_left);
  display.setTextFont(2);
  display.setTextSize(1);
  display.drawString("PresenceCard Display", 24, 14);

  display.setTextColor(kMuted, kBackground);
  display.drawString(deviceId.length() ? deviceId : "dvc_pending", 24, 46);

  display.setTextDatum(lgfx::textdatum_t::top_right);
  display.setTextColor(kInk, kBackground);
  display.drawString(statusText, 776, 14);
  display.setTextColor(kMuted, kBackground);
  display.drawString(statusDetail, 776, 46);

  display.setTextDatum(lgfx::textdatum_t::middle_left);
  display.setTextColor(kMuted, kBackground);
  display.drawString("PAIR", 24, 450);
  display.setTextColor(kInk, kBackground);
  display.setTextSize(2);
  display.drawString(pairCode.length() == 6 ? pairCode : (token.length() ? "PAIRED" : "------"),
                     92, 450);
  display.setTextSize(1);
  display.endWrite();
}

void showPanelMessage(const String& title, const String& detail,
                      uint32_t accent = kPink) {
  display.startWrite();
  display.fillScreen(kBackground);
  display.fillRoundRect(120, 135, 560, 210, 24, accent);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(kInk, accent);
  display.setTextFont(2);
  display.setTextSize(2);
  display.drawString(title, 400, 205);
  display.setTextSize(1);
  display.setTextColor(kMuted, accent);
  display.drawString(detail, 400, 275);
  display.endWrite();
  drawChrome();
}

void setStatus(const String& status, const String& detail = String()) {
  statusText = status;
  statusDetail = detail;
  // Once a photo is visible, keep it full-screen. Runtime status remains on
  // serial and the chrome returns only when pairing/configuration is needed.
  if (!hasRenderedPhoto) drawChrome();
  Serial.printf("STATUS %s %s\n", status.c_str(), detail.c_str());
}

void showColorCheck() {
  Serial.println("COLOR_CHECK left=RED center=GREEN right=BLUE");
  display.startWrite();
  display.fillRect(0, 0, 267, 480, TFT_RED);
  display.fillRect(267, 0, 266, 480, TFT_GREEN);
  display.fillRect(533, 0, 267, 480, TFT_BLUE);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(TFT_WHITE);
  display.setTextFont(2);
  display.setTextSize(2);
  display.drawString("RED", 133, 240);
  display.drawString("GREEN", 400, 240);
  display.drawString("BLUE", 666, 240);
  display.endWrite();
  delay(1200);
}

String makeDeviceId() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char value[24];
  snprintf(value, sizeof(value), "dvc_%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(value);
}

bool beginHttp(HTTPClient& http, WiFiClientSecure& secureClient,
               WiFiClient& plainClient, const String& url) {
  if (!urlAllowed(url)) return false;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (url.startsWith("https://")) {
    secureClient.setCACert(PVC_TLS_ROOT_CA_PEM);
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

HttpResponse requestJson(const char* method, const String& path,
                         const String& body = String(), bool authenticated = true) {
  HttpResponse response;
  const String url = endpointUrl(path);
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (!beginHttp(http, secureClient, plainClient, url)) {
    Serial.printf("HTTP BLOCKED %s\n", url.c_str());
    return response;
  }

  http.addHeader("Accept", "application/json");
  if (authenticated) http.addHeader("Authorization", "Bearer " + token);
  const uint32_t started = millis();
  if (strcmp(method, "GET") == 0) {
    response.code = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    response.code = http.POST(body);
  }

  const int contentLength = http.getSize();
  if (response.code > 0 && contentLength <= static_cast<int>(kMaxJsonBytes)) {
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(
        kMaxJsonBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer != nullptr) {
      BoundedWriteStream sink(buffer, kMaxJsonBytes);
      const int written = http.writeToStream(&sink);
      const bool lengthMatches =
          contentLength < 0 || sink.size() == static_cast<size_t>(contentLength);
      if (written >= 0 && !sink.overflowed() && lengthMatches) {
        buffer[sink.size()] = '\0';
        response.body = String(buffer, static_cast<unsigned int>(sink.size()));
      } else {
        Serial.printf("JSON BODY REJECTED declared=%d received=%u overflow=%d\n",
                      contentLength, static_cast<unsigned>(sink.size()),
                      sink.overflowed());
      }
      heap_caps_free(buffer);
    } else {
      Serial.println("JSON BODY REJECTED allocation failed");
    }
  } else if (response.code > 0) {
    Serial.printf("JSON BODY REJECTED declared=%d limit=%u\n", contentLength,
                  static_cast<unsigned>(kMaxJsonBytes));
  }
  Serial.printf("%s %s %d %lums bytes=%u\n", method, path.c_str(), response.code,
                static_cast<unsigned long>(millis() - started),
                static_cast<unsigned>(response.body.length()));
  http.end();
  return response;
}

void clearBinding() {
  token = String();
  pairCode = String();
  preferences.remove("token");
  nextPairActionAt = 0;
  hasRenderedPhoto = false;
  showPanelMessage("PAIRING REQUIRED", "requesting a new 6-digit code", kPink);
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  const uint32_t now = millis();
  if (!timeReached(now, nextWifiAttemptAt)) return false;

  setStatus("WIFI CONNECTING", PVC_WIFI_SSID);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(PVC_WIFI_SSID, PVC_WIFI_PASSWORD);
  const uint32_t deadline = millis() + 12000;
  while (WiFi.status() != WL_CONNECTED && !timeReached(millis(), deadline)) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    nextWifiAttemptAt = millis() + kWifiRetryMs;
    setStatus("WIFI OFFLINE", "retrying");
    return false;
  }
  setStatus("WIFI ONLINE", WiFi.localIP().toString());
  return true;
}

bool ensureTlsClock() {
  if (!apiBase.startsWith("https://")) return true;
  time_t currentTime = 0;
  time(&currentTime);
  if (currentTime >= 1704067200) return true;  // 2024-01-01 UTC

  const uint32_t now = millis();
  if (!timeReached(now, nextTimeAttemptAt)) return false;
  setStatus("SYNCING CLOCK", "required for TLS validation");
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  const uint32_t deadline = millis() + 10000;
  while (!timeReached(millis(), deadline)) {
    time(&currentTime);
    if (currentTime >= 1704067200) {
      setStatus("CLOCK READY", "TLS certificate dates enabled");
      return true;
    }
    delay(100);
  }
  nextTimeAttemptAt = millis() + kWifiRetryMs;
  setStatus("CLOCK OFFLINE", "TLS requests remain blocked");
  return false;
}

bool validPairCode(const char* value) {
  if (value == nullptr || strlen(value) != 6) return false;
  for (size_t i = 0; i < 6; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

bool requestPairCode() {
  JsonDocument request;
  request["device_id"] = deviceId;
  request["fw_version"] = kFirmwareVersion;
  request["hw"] = kHardwareName;
  String body;
  serializeJson(request, body);

  const HttpResponse response = requestJson("POST", "/pair/code", body, false);
  if (response.code != 200) return false;
  JsonDocument document;
  const DeserializationError jsonError = deserializeJson(document, response.body);
  if (jsonError) {
    Serial.printf("PAIR JSON ERROR %s\n", jsonError.c_str());
    return false;
  }
  const char* code = document["pair_code"].as<const char*>();
  if (!validPairCode(code)) {
    Serial.println("PAIR CODE INVALID");
    return false;
  }
  pairCode = code;
  Serial.printf("PAIR CODE %s\n", pairCode.c_str());
  showPanelMessage(pairCode, "enter this code on the PresenceCard web app", kBlue);
  setStatus("WAITING FOR PAIR", "code valid for server expiry");
  return true;
}

void pollPairStatus() {
  String path = "/pair/status?device_id=" + deviceId + "&pair_code=" + pairCode;
  const HttpResponse response = requestJson("GET", path, String(), false);
  if (response.code == 202) {
    setStatus("WAITING FOR PAIR", "polling every 3 seconds");
    return;
  }
  if (response.code == 410) {
    pairCode = String();
    setStatus("PAIR CODE EXPIRED", "requesting a new code");
    return;
  }
  if (response.code != 200) {
    setStatus("PAIR API ERROR", String(response.code));
    return;
  }

  JsonDocument document;
  if (deserializeJson(document, response.body)) return;
  const char* state = document["status"] | "";
  const char* newToken = document["device_token"] | "";
  if (strcmp(state, "bound") != 0 || strlen(newToken) == 0) return;

  token = newToken;
  preferences.putString("token", token);
  pairCode = String();
  setStatus("PAIRED", "token saved in NVS");
  nextCloudPollAt = 0;
}

bool isSofMarker(uint8_t marker) {
  switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
      return true;
    default:
      return false;
  }
}

bool jpegDimensions(const uint8_t* data, size_t length, uint16_t& width,
                    uint16_t& height) {
  if (data == nullptr || length < 4 || data[0] != 0xFF || data[1] != 0xD8 ||
      data[length - 2] != 0xFF || data[length - 1] != 0xD9) {
    return false;
  }

  size_t position = 2;
  while (position + 4 <= length) {
    while (position < length && data[position] != 0xFF) ++position;
    while (position < length && data[position] == 0xFF) ++position;
    if (position >= length) return false;
    const uint8_t marker = data[position++];
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (position + 2 > length) return false;
    const uint16_t segmentLength =
        (static_cast<uint16_t>(data[position]) << 8) | data[position + 1];
    if (segmentLength < 2 || position + segmentLength > length) return false;
    if (isSofMarker(marker)) {
      if (segmentLength < 7) return false;
      height = (static_cast<uint16_t>(data[position + 3]) << 8) | data[position + 4];
      width = (static_cast<uint16_t>(data[position + 5]) << 8) | data[position + 6];
      return width > 0 && height > 0;
    }
    position += segmentLength;
  }
  return false;
}

JpegDownload downloadJpeg(const String& url) {
  JpegDownload result;
  if (!sameOrigin(apiBase, url)) {
    Serial.printf("IMAGE BLOCKED cross-origin %s\n", url.c_str());
    return result;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  if (!beginHttp(http, secureClient, plainClient, url)) return result;
  static const char* headers[] = {"Content-Type"};
  http.collectHeaders(headers, 1);
  http.addHeader("Accept", "image/jpeg");
  http.addHeader("Authorization", "Bearer " + token);
  const uint32_t started = millis();
  result.httpCode = http.GET();
  const int declaredLength = http.getSize();
  String contentType = http.header("Content-Type");
  contentType.toLowerCase();
  if (result.httpCode != 200 || !contentType.startsWith("image/jpeg") ||
      declaredLength > static_cast<int>(kMaxJpegBytes)) {
    Serial.printf("GET image %d %lums type=%s size=%d\n", result.httpCode,
                  static_cast<unsigned long>(millis() - started), contentType.c_str(),
                  declaredLength);
    http.end();
    return result;
  }

  result.data = static_cast<uint8_t*>(
      heap_caps_malloc(kMaxJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (result.data == nullptr) {
    http.end();
    return result;
  }

  BoundedWriteStream sink(result.data, kMaxJpegBytes);
  const int written = http.writeToStream(&sink);
  result.size = sink.size();
  Serial.printf("GET image %d %lums bytes=%u\n", result.httpCode,
                static_cast<unsigned long>(millis() - started),
                static_cast<unsigned>(result.size));
  http.end();

  if (written < 0 || sink.overflowed() || result.size == 0 ||
      (declaredLength >= 0 && result.size != static_cast<size_t>(declaredLength))) {
    heap_caps_free(result.data);
    result.data = nullptr;
    result.size = 0;
  }
  return result;
}

bool decodeAndDrawJpeg(uint8_t* data, size_t size) {
  uint16_t width = 0;
  uint16_t height = 0;
  if (!jpegDimensions(data, size, width, height) || width != kExpectedJpegWidth ||
      height != kExpectedJpegHeight) {
    Serial.printf("JPEG REJECTED dimensions=%ux%u bytes=%u\n", width, height,
                  static_cast<unsigned>(size));
    return false;
  }

  // 320x240 -> 800x600, then crop 60 px from the scaled top and bottom. This
  // fills the 800x480 panel without stretching the photo.
  constexpr float kCoverScale = 2.5f;
  constexpr int kScaledVerticalCrop = 60;
  display.startWrite();
  display.fillScreen(TFT_BLACK);
  const bool decoded = display.drawJpg(data, size, 0, 0, 800, 480, 0,
                                       kScaledVerticalCrop, kCoverScale,
                                       kCoverScale);
  display.endWrite();
  return decoded;
}

void processFeed(const HttpResponse& response) {
  if (response.code == 401) {
    clearBinding();
    return;
  }
  if (response.code != 200) {
    setStatus("FEED ERROR", String(response.code));
    return;
  }

  JsonDocument document;
  if (deserializeJson(document, response.body)) {
    setStatus("FEED INVALID", "JSON parse failed");
    return;
  }
  JsonArray items = document["items"].as<JsonArray>();
  if (items.isNull() || items.size() == 0) {
    setStatus("ONLINE", "feed is empty");
    return;
  }
  JsonObject item = items[0];
  const char* id = item["photo_id"] | "";
  const char* urlValue = item["image_url"] | "";
  if (strlen(id) == 0 || strlen(urlValue) == 0) {
    setStatus("FEED INVALID", "missing photo_id/image_url");
    return;
  }
  if (latestPhotoId == id && hasRenderedPhoto) {
    setStatus("ONLINE", "latest photo is current");
    return;
  }

  setStatus("DOWNLOADING", id);
  const String url = imageUrl(urlValue);
  JpegDownload jpeg = downloadJpeg(url);
  if (jpeg.httpCode == 401) {
    if (jpeg.data != nullptr) heap_caps_free(jpeg.data);
    clearBinding();
    return;
  }
  if (jpeg.data == nullptr) {
    setStatus("IMAGE ERROR", "download rejected");
    return;
  }

  const bool rendered = decodeAndDrawJpeg(jpeg.data, jpeg.size);
  heap_caps_free(jpeg.data);
  if (!rendered) {
    setStatus("IMAGE ERROR", "JPEG rejected or decode failed");
    return;
  }

  latestPhotoId = id;
  preferences.putString("latest_photo", latestPhotoId);
  hasRenderedPhoto = true;
  const char* author = item["author"]["display_name"] | "new photo";
  setStatus("PHOTO READY", author);
}

void pollCloud() {
  const HttpResponse state = requestJson("GET", "/device/state");
  if (state.code == 401) {
    clearBinding();
    return;
  }
  if (state.code == 200) {
    JsonDocument document;
    if (!deserializeJson(document, state.body)) {
      const int unseen = document["unseen_count"] | 0;
      const int pending = document["pending_friend_requests"] | 0;
      statusDetail = "unseen " + String(unseen) + " / requests " + String(pending);
      if (!hasRenderedPhoto) drawChrome();
    }
  } else {
    setStatus("STATE ERROR", String(state.code));
  }

  // Deliberately independent from unseen_count: current servers may not update
  // that field reliably, while /feed remains the source of truth for the photo.
  processFeed(requestJson("GET", "/feed?limit=1"));
}

bool validateConfiguration() {
  if (strlen(PVC_WIFI_SSID) == 0 || apiBase.length() == 0) return false;
  if (!urlAllowed(apiBase) || apiBase.indexOf('?') >= 0) return false;
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);

  displayReady = display.init();
  if (!displayReady) {
    Serial.println("DISPLAY INIT FAILED");
    return;
  }
  display.setBrightness(180);
  showColorCheck();

  deviceId = makeDeviceId();
  apiBase = normalizedBaseUrl();
  preferences.begin("pvc_display", false);
  token = preferences.getString("token", "");
  latestPhotoId = preferences.getString("latest_photo", "");
  psramReady = psramFound() && ESP.getPsramSize() >= kMinPsramBytes;
  configReady = validateConfiguration();

  Serial.printf("DEVICE %s PSRAM=%u API=%s\n", deviceId.c_str(),
                static_cast<unsigned>(ESP.getPsramSize()), apiBase.c_str());
  if (!psramReady) {
    statusText = "HARDWARE REQUIRED";
    statusDetail = "8MB PSRAM not detected";
    showPanelMessage("PSRAM REQUIRED", "expected ESP32-S3 N16R8", kPink);
    return;
  }
  if (!configReady) {
    statusText = "CONFIG REQUIRED";
    statusDetail = "run tools/configure.py";
    showPanelMessage("CONFIG REQUIRED", "Wi-Fi, API URL and HTTPS CA", kPink);
    return;
  }

  if (token.length()) {
    statusText = "PAIRED";
    statusDetail = "binding restored from NVS";
    showPanelMessage("READY", "connecting to PresenceCard", kSage);
  } else {
    statusText = "PAIRING";
    statusDetail = "waiting for network";
    showPanelMessage("PAIRING", "a 6-digit code will appear here", kBlue);
  }
}

void loop() {
  if (!displayReady || !configReady || !psramReady) {
    delay(250);
    return;
  }
  if (!ensureWifi()) {
    delay(50);
    return;
  }
  if (!ensureTlsClock()) {
    delay(50);
    return;
  }

  const uint32_t now = millis();
  if (token.length() == 0) {
    if (!timeReached(now, nextPairActionAt)) {
      delay(25);
      return;
    }
    if (pairCode.length() == 0) {
      const bool requested = requestPairCode();
      nextPairActionAt = now + (requested ? kPairPollMs : kPairRetryMs);
    } else {
      pollPairStatus();
      nextPairActionAt = now + kPairPollMs;
    }
  } else if (timeReached(now, nextCloudPollAt)) {
    nextCloudPollAt = now + kCloudPollMs;
    pollCloud();
  }
  delay(25);
}
