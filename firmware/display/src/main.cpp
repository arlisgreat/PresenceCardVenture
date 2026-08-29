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
// A single 401 no longer wipes the binding (transient server bugs used to
// cause self-unbinding loops). Only this many *consecutive* 401s, with no
// successful request in between, prove the token is really dead.
constexpr uint8_t kAuthFailureLimit = 5;

// Design tokens from the UI spec (Presence小卡-屏幕UI与交互量化设计规范).
constexpr uint32_t kBackground = 0xFDFBF5;  // paper_white
constexpr uint32_t kInk = 0x242529;
constexpr uint32_t kMuted = 0x74777D;
constexpr uint32_t kPink = 0xF7A8C8;   // candy_pink: like highlight, pair success
constexpr uint32_t kBlue = 0x2F7DE0;   // accent_sky_blue: pair code, timestamps
constexpr uint32_t kSage = 0x3E7A3A;   // grass_green: pairing/settings panels
constexpr uint32_t kLilac = 0xC7B8EE;  // lilac_purple: arrival nickname sticker
constexpr uint32_t kNight = 0x1A1D1A;  // dark_night: chrome + selector backdrop
constexpr uint32_t kShadow = 0x0A0C0A; // hard drop shadow under paper cards

// Arrival ritual (photo-arrival ceremony). A full-screen alpha blend would not
// fit the PSRAM bandwidth budget, so the fade rides the backlight PWM instead.
constexpr uint32_t kArrivalTtlMs = 20000;    // photo stays full-screen 20 s
constexpr uint32_t kArrivalFadeMs = 500;     // backlight ramp in/out
constexpr uint32_t kBrightnessTickMs = 25;   // loop cadence for PWM updates
constexpr uint8_t kBrightnessBoot = 180;
constexpr uint8_t kBrightnessArrivalLow = 77;    // ~30%: fade-in start
constexpr uint8_t kBrightnessArrivalFull = 255;  // 100%: arrival peak
constexpr uint8_t kBrightnessResident = 179;     // ~70%: resident state after TTL
constexpr uint8_t kBrightnessBrowseDim = 0;      // page turn: full black while loading
constexpr uint32_t kBrowseRampMs = 150;          // then the new photo fades up
// 用户要求: 翻页黑屏+亮起总时长 ≤0.5s。缓存命中时黑屏只剩解码耗时,
// 150ms 渐亮把整个过渡压进预算内。
constexpr int16_t kStickerX = 8;
constexpr int16_t kStickerY = 8;
constexpr int16_t kStickerHeight = 32;
constexpr int16_t kStickerMaxWidth = 300;
// 320x240 -> 800x600, then crop 60 px from the scaled top and bottom. This
// fills the 800x480 panel without stretching the photo.
constexpr float kCoverScale = 2.5f;
constexpr int kScaledVerticalCrop = 60;

// ---- Touch gesture engine (GT911) ------------------------------------------
// Non-blocking state machine fed by getTouch() polling in loop(). It only
// *emits* events; consumers (carousel = SWIPE_LEFT/RIGHT, screen-off =
// SWIPE_UP, settings = LONG_PRESS) pop them via nextGestureEvent().
constexpr uint32_t kStarYellow = 0xF5D76E;  // cream_yellow: like star burst
constexpr int kTapMaxMovePx = 12;         // tap: displacement below this (GT911 抖动余量)
constexpr uint32_t kTapMaxMs = 350;       // tap: press shorter than this
constexpr uint32_t kMultiTapGapMs = 300;  // taps closer than this are a chain
constexpr int kSwipeMinMovePx = 30;       // swipe: displacement above this
constexpr uint32_t kSwipeMaxMs = 900;     // swipe: 慢滑也算 (400ms 丢掉了一半真实滑动)
constexpr uint32_t kLongPressMs = 1500;   // long press: hold at least this
constexpr uint32_t kLikeWindowMs = 2000;  // like aggregation silence window
constexpr uint8_t kGestureQueueSize = 8;
constexpr uint32_t kStarLifetimeMs = 1000;
constexpr uint8_t kMaxStars = 8;  // ring buffer of simultaneous particles
constexpr int16_t kStarBufPx = 40;  // save-under square per star (RGB565)

// ---- Display modes (long-press selector) + info bar --------------------------
constexpr uint32_t kSelectorTimeoutMs = 4000;  // auto-cancel when ignored
constexpr uint32_t kSlideshowIntervalMs = 5000;   // 常开自动轮播: 5 秒一页
constexpr uint32_t kAutoSlideTouchHoldMs = 15000;  // 任何触摸后暂停轮播 15 秒
constexpr uint32_t kPollTouchHoldMs = 6000;   // 触摸活跃期不发起同步轮询 (防卡手)
constexpr uint32_t kPrefetchIdleMs = 2500;    // 空闲这么久后才允许预取 feed JPEG
constexpr uint32_t kPrefetchRetryMs = 6000;   // 预取失败的退避
constexpr uint32_t kModeToastMs = 1500;
constexpr int16_t kCaptionBarHeight = 64;  // "polaroid chin" info bar
constexpr int16_t kCaptionBarY = 480 - kCaptionBarHeight;

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
uint8_t consecutiveAuthFailures = 0;

// Arrival ritual state machine (all timing is millis()-based, non-blocking).
enum class ArrivalPhase : uint8_t { Idle, FadeIn, Hold, FadeOut };
ArrivalPhase arrivalPhase = ArrivalPhase::Idle;
uint32_t arrivalShownAt = 0;             // millis() when the photo hit the screen
uint32_t arrivalPhaseStartedAt = 0;      // millis() when the current fade began
uint32_t arrivalLastBrightnessTickAt = 0;
uint8_t* shownJpeg = nullptr;            // JPEG of the photo currently on screen,
size_t shownJpegSize = 0;                // retained in PSRAM for overlay cleanup
int16_t arrivalStickerWidth = 0;         // actual sticker width for the repaint clip
bool arrivalCaptionShown = false;        // caption bar needs cleanup at TTL end

// ---- Gesture engine state ---------------------------------------------------
enum class GestureEvent : uint8_t {
  NONE = 0,
  TAP,
  SWIPE_LEFT,
  SWIPE_RIGHT,
  SWIPE_UP,
  SWIPE_DOWN,
  LONG_PRESS,
};

struct QueuedGesture {
  GestureEvent event = GestureEvent::NONE;
  int16_t x = 0;
  int16_t y = 0;
};

QueuedGesture gestureQueue[kGestureQueueSize];
uint8_t gestureQueueHead = 0;  // next slot to read
uint8_t gestureQueueTail = 0;  // next slot to write

bool touchHeld = false;
bool longPressFired = false;
int16_t touchDownX = 0;
int16_t touchDownY = 0;
int16_t touchLastX = 0;
int16_t touchLastY = 0;
uint32_t touchDownAt = 0;

uint8_t pendingLikeTaps = 0;
uint32_t lastLikeTapAt = 0;
uint8_t likeTapChain = 0;  // consecutive taps within kMultiTapGapMs, for logs

struct StarParticle {
  bool active = false;
  int16_t x = 0;
  int16_t y = 0;
  uint32_t startedAt = 0;
  uint8_t lastStep = 255;
  // Save-under state: pixels beneath the previous frame, restored each step so
  // expired stars leave no trace on the photo.
  int16_t savedX = 0;
  int16_t savedY = 0;
  int16_t savedW = 0;
  int16_t savedH = 0;
  uint16_t* saved = nullptr;  // kStarBufPx^2 RGB565 buffer, lazily in PSRAM
};

StarParticle stars[kMaxStars];
uint8_t starWriteIndex = 0;

// ---- Carousel (multi-photo feed) state ---------------------------------------
// Metadata plus a per-slot JPEG cache in PSRAM (5 x <=160 KiB = <=800 KiB,
// comfortably inside the 8 MiB part). Cache hits make swipes render without
// touching the network, which is what keeps the page turn under 0.5 s.
constexpr size_t kFeedLimit = 5;
struct FeedPhoto {
  String photoId;
  String imageUrl;
  String author;
  String caption;
  uint8_t* jpeg = nullptr;  // owned, right-sized PSRAM copy (nullptr = not cached)
  size_t jpegSize = 0;
};
FeedPhoto feedPhotos[kFeedLimit];
size_t feedCount = 0;
size_t currentPhotoIndex = 0;  // 0 = newest
uint32_t lastUserTouchAt = 0;   // any finger contact; gates polling + slideshow
bool pollForcedByGesture = false;  // SWIPE_DOWN refresh bypasses the touch gate
uint32_t nextPrefetchAt = 0;

// ---- Display modes (long-press selector) -------------------------------------
// Latest: new arrivals take over (default). Resident: pinned photo returns
// after each arrival ritual. Slideshow: auto-advance through the feed.
enum class DisplayMode : uint8_t { Latest = 0, Resident = 1, Slideshow = 2 };
DisplayMode displayMode = DisplayMode::Latest;
String residentPhotoId;  // pinned photo while in Resident mode
bool selectorOpen = false;
uint32_t selectorOpenedAt = 0;
uint32_t nextSlideAt = 0;
bool modeToastActive = false;
uint32_t modeToastUntil = 0;

// ---- Sleep mode + generic backlight ramp state -------------------------------
// startBacklightRamp()/updateBacklightRamp() below are generic and shared;
// other workstreams can reuse them instead of a custom brightness ticker.
constexpr uint32_t kSleepRampMs = 800;  // ease-in dim to 0 on SWIPE_UP
constexpr uint32_t kWakeRampMs = 500;   // ease-out back up on wake tap
bool sleeping = false;
uint8_t missedWhileSleeping = 0;
uint8_t currentBrightness = kBrightnessBoot;
bool blRampActive = false;
bool blRampEaseIn = false;
uint8_t blRampFrom = 0;
uint8_t blRampTo = 0;
uint32_t blRampStartMs = 0;
uint32_t blRampDurationMs = 1;
// Multi-consumer-safe read cursor into gestureQueue: carousel/sleep tracks its
// own cursor instead of popping the shared head, so the like/star consumer
// still sees TAP events. Queue overflow discipline stays with the engine owner.
uint8_t carouselGestureCursor = 0;

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

// ---- "Handmade polaroid" chrome ---------------------------------------------
// The pairing / status screen is the first thing a new owner sees, so it gets
// the full design language: dark_night backdrop, one paper card with a hard
// drop shadow, slanted washi-tape corners, and the 6-digit code as the hero.

// Slanted parallelogram strip — the cheap stand-in for a rotated tape sticker.
void drawWashiTape(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
  const int16_t slant = h / 2;
  display.fillTriangle(x + slant, y, x + w, y, x, y + h, color);
  display.fillTriangle(x + w, y, x + w - slant, y + h, x, y + h, color);
}

// Small six-point star (two overlapped triangles), used as an accent glyph.
void drawStarGlyph(int16_t cx, int16_t cy, int16_t r, uint32_t color) {
  display.fillTriangle(cx, cy - r, cx - r, cy + r / 2, cx + r, cy + r / 2, color);
  display.fillTriangle(cx, cy + r, cx - r, cy - r / 2, cx + r, cy - r / 2, color);
}

// Forces the next drawChrome() to repaint the whole screen (statics below
// otherwise limit the 3 s poll cadence to a status-strip refresh).
String chromeStateShown = "\x01";

void drawChromeStatusStrip() {
  display.startWrite();
  display.fillRect(140, 150, 520, 28, kBackground);
  display.setFont(&fonts::efontCN_16);
  display.setTextSize(1);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(kMuted, kBackground);
  String line = statusText;
  if (statusDetail.length()) {
    line += "  ·  ";
    line += statusDetail;
  }
  while (line.length() && display.textWidth(line) > 500) {
    line.remove(line.length() - 1);
  }
  display.drawString(line, 400, 164);
  display.endWrite();
}

void drawChrome() {
  const String state =
      token.length() ? String("bound")
                     : (pairCode.length() == 6 ? pairCode : String("waiting"));
  if (state == chromeStateShown) {  // only the status text changed
    drawChromeStatusStrip();
    return;
  }
  chromeStateShown = state;

  display.startWrite();
  display.fillScreen(kNight);
  // Paper card with a hard shadow.
  display.fillRoundRect(102, 64, 608, 368, 24, kShadow);
  display.fillRoundRect(96, 56, 608, 368, 24, kBackground);
  // Washi tape over the two top corners of the card.
  drawWashiTape(140, 44, 96, 26, kLilac);
  drawWashiTape(556, 44, 96, 26, kPink);

  display.setFont(&fonts::efontCN_16);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextSize(2);
  display.setTextColor(kInk, kBackground);
  display.drawString("Presence 相框", 400, 112);
  drawStarGlyph(268, 112, 8, kStarYellow);
  drawStarGlyph(532, 112, 8, kStarYellow);

  if (token.length()) {
    // Bound: green check medallion instead of the code boxes.
    display.fillCircle(400, 240, 40, kSage);
    display.fillCircle(400, 240, 34, kBackground);
    display.fillCircle(400, 240, 28, kSage);
    // Chunky check mark from two thick strokes.
    for (int i = -2; i <= 2; ++i) {
      display.drawLine(384, 240 + i, 396, 252 + i, kBackground);
      display.drawLine(396, 252 + i, 418, 228 + i, kBackground);
    }
    display.setTextSize(1);
    display.setTextColor(kInk, kBackground);
    display.drawString("已绑定 · 照片正在路上", 400, 316);
  } else {
    // Six digit boxes, the hero of the screen.
    const int16_t boxW = 74, boxH = 96, gap = 12;
    const int16_t x0 = (800 - (6 * boxW + 5 * gap)) / 2;
    for (int i = 0; i < 6; ++i) {
      const int16_t bx = x0 + i * (boxW + gap);
      display.fillRoundRect(bx, 196, boxW, boxH, 10, kNight);
      if (pairCode.length() == 6) {
        display.setTextSize(4);
        display.setTextColor(kBackground, kNight);
        display.drawString(String(pairCode[i]), bx + boxW / 2, 196 + boxH / 2);
      } else {
        display.fillCircle(bx + boxW / 2, 196 + boxH / 2, 5, kMuted);
      }
    }
    display.setTextSize(1);
    display.setTextColor(kMuted, kBackground);
    display.drawString("打开 App · 添加设备 · 输入这串配对码", 400, 326);
  }

  // Hairline + device id footer inside the card.
  display.fillRect(140, 372, 520, 2, 0xE8E4DA);
  display.setTextSize(1);
  display.setTextColor(kMuted, kBackground);
  display.drawString(deviceId.length() ? deviceId : "dvc_pending", 400, 398);
  display.endWrite();

  drawChromeStatusStrip();
}

void showPanelMessage(const String& title, const String& detail,
                      uint32_t accent = kPink) {
  chromeStateShown = "\x01";  // next drawChrome() repaints from scratch
  display.startWrite();
  display.fillScreen(kNight);
  display.fillRoundRect(126, 156, 548, 188, 24, kShadow);
  display.fillRoundRect(120, 148, 548, 188, 24, kBackground);
  drawWashiTape(160, 136, 96, 26, accent);
  display.setFont(&fonts::efontCN_16);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(kInk, kBackground);
  display.setTextSize(2);
  display.drawString(title, 400, 218);
  display.setTextSize(1);
  display.setTextColor(kMuted, kBackground);
  display.drawString(detail, 400, 282);
  display.endWrite();
}

void setStatus(const String& status, const String& detail = String()) {
  statusText = status;
  statusDetail = detail;
  // Once a photo is visible, keep it full-screen. Runtime status remains on
  // serial and the chrome returns only when pairing/configuration is needed.
  if (!hasRenderedPhoto) drawChrome();
  Serial.printf("STATUS %s %s\n", status.c_str(), detail.c_str());
}

// ---- Arrival ritual -------------------------------------------------------

// Ease-out approximation of cubic-bezier(0.25, 1, 0.5, 1): cheap 1-(1-t)^2.
float easeOutQuad(float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const float u = 1.0f - t;
  return 1.0f - u * u;
}

uint8_t lerpBrightness(uint8_t from, uint8_t to, float t) {
  const float eased = easeOutQuad(t);
  return static_cast<uint8_t>(from + (static_cast<int>(to) - from) * eased);
}

// Nickname sticker at the top-left corner. LovyanGFX cannot rotate small
// elements cheaply, so the sticker feel comes from a 2 px hard shadow
// (spec-sanctioned fallback for the -2 degree rotation).
void drawNameSticker(const String& author) {
  String label = author.length() ? author : String("friend");
  display.setFont(&fonts::efontCN_16);  // CJK-capable: friends have Chinese names
  display.setTextSize(1);
  int16_t textWidth = display.textWidth(label);
  int16_t width = textWidth + 16;  // 8 px padding on each side
  while (width > kStickerMaxWidth && label.length() > 1) {
    label.remove(label.length() - 1);
    textWidth = display.textWidth(label);
    width = textWidth + 16;
  }
  arrivalStickerWidth = width;

  display.startWrite();
  display.fillRoundRect(kStickerX + 2, kStickerY + 2, width, kStickerHeight, 4,
                        kInk);  // hard shadow
  display.fillRoundRect(kStickerX, kStickerY, width, kStickerHeight, 4, kLilac);
  display.drawRoundRect(kStickerX, kStickerY, width, kStickerHeight, 4,
                        TFT_WHITE);  // 1 px white border
  display.setTextDatum(lgfx::textdatum_t::middle_left);
  display.setTextColor(kInk, kLilac);
  display.drawString(label, kStickerX + 8, kStickerY + kStickerHeight / 2);
  display.endWrite();
}

// Backlight entry point that keeps the sleep/ramp bookkeeping in sync;
// defined below with the sleep-mode helpers.
void setBacklightNow(uint8_t value);

void freeShownJpeg() {
  if (shownJpeg != nullptr) {
    heap_caps_free(shownJpeg);
    shownJpeg = nullptr;
    shownJpegSize = 0;
  }
}

// Take ownership of a downloaded JPEG as "what is on screen right now". The
// retained bytes let overlays (sticker, caption bar, selector, toast) be
// erased by re-decoding just their clip region instead of re-downloading.
void retainShownJpeg(uint8_t* data, size_t size) {
  if (shownJpeg == data) return;
  freeShownJpeg();
  shownJpeg = data;
  shownJpegSize = size;
}

// ---- Per-slot feed JPEG cache (PSRAM) ---------------------------------------
void freeFeedJpeg(FeedPhoto& slot) {
  if (slot.jpeg != nullptr) {
    heap_caps_free(slot.jpeg);
    slot.jpeg = nullptr;
    slot.jpegSize = 0;
  }
}

// Store a right-sized copy so the slot survives the caller freeing its buffer.
void cacheFeedJpeg(size_t index, const uint8_t* data, size_t size) {
  if (index >= kFeedLimit || data == nullptr || size == 0) return;
  FeedPhoto& slot = feedPhotos[index];
  freeFeedJpeg(slot);
  uint8_t* copy = static_cast<uint8_t*>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy == nullptr) return;  // cache miss stays a miss; nothing breaks
  memcpy(copy, data, size);
  slot.jpeg = copy;
  slot.jpegSize = size;
}

// Abandon any in-flight ritual (binding cleared, next photo takes over).
void resetArrival(bool restoreBrightness) {
  arrivalPhase = ArrivalPhase::Idle;
  arrivalCaptionShown = false;
  if (restoreBrightness) setBacklightNow(kBrightnessBoot);
}

// Repaint a rectangle of the screen from the retained JPEG so overlays can be
// erased without a full redraw or a re-download.
void repaintRegionFromShown(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (shownJpeg == nullptr) return;
  display.startWrite();
  display.setClipRect(x, y, w, h);
  display.drawJpg(shownJpeg, shownJpegSize, 0, 0, 800, 480, 0,
                  kScaledVerticalCrop, kCoverScale, kCoverScale);
  display.clearClipRect();
  display.endWrite();
}

void repaintFullFromShown() {
  if (shownJpeg == nullptr) return;
  display.startWrite();
  display.drawJpg(shownJpeg, shownJpegSize, 0, 0, 800, 480, 0,
                  kScaledVerticalCrop, kCoverScale, kCoverScale);
  display.endWrite();
}

// Repaint only the sticker area from the retained JPEG so the sticker
// disappears at the end of the TTL while the photo stays on screen.
void repaintStickerRegion() {
  if (arrivalStickerWidth <= 0) return;
  repaintRegionFromShown(kStickerX, kStickerY, arrivalStickerWidth + 3,
                         kStickerHeight + 3);
}

// Called once when a new photo is on screen: log the event and start the
// 500 ms backlight fade-in. Assumes brightness was already dropped low and
// the sticker drawn.
void beginArrival(const char* photoId, const char* author) {
  Serial.printf("STATUS ARRIVAL %s by %s\n", photoId, author);
  const uint32_t now = millis();
  arrivalShownAt = now;
  arrivalPhaseStartedAt = now;
  arrivalLastBrightnessTickAt = now;
  arrivalPhase = ArrivalPhase::FadeIn;
}

// Non-blocking backlight state machine, called from loop().
void onArrivalRetired();  // defined below the carousel section
void updateArrival(uint32_t now) {
  if (arrivalPhase == ArrivalPhase::Idle) return;

  if (arrivalPhase == ArrivalPhase::Hold) {
    // Fade-out starts 500 ms before the 20 s TTL ends.
    if (now - arrivalShownAt >= kArrivalTtlMs - kArrivalFadeMs) {
      arrivalPhase = ArrivalPhase::FadeOut;
      arrivalPhaseStartedAt = now;
      arrivalLastBrightnessTickAt = now;
    }
    return;
  }

  if (!timeReached(now, arrivalLastBrightnessTickAt + kBrightnessTickMs)) {
    return;
  }
  arrivalLastBrightnessTickAt = now;
  const float t =
      static_cast<float>(now - arrivalPhaseStartedAt) / kArrivalFadeMs;

  if (arrivalPhase == ArrivalPhase::FadeIn) {
    if (t >= 1.0f) {
      setBacklightNow(kBrightnessArrivalFull);
      arrivalPhase = ArrivalPhase::Hold;
    } else {
      setBacklightNow(
          lerpBrightness(kBrightnessArrivalLow, kBrightnessArrivalFull, t));
    }
    return;
  }

  // FadeOut: 100% -> 70% resident level, then retire the ritual.
  if (t >= 1.0f) {
    setBacklightNow(kBrightnessResident);
    repaintStickerRegion();
    if (arrivalCaptionShown) {
      repaintRegionFromShown(0, kCaptionBarY, 800, kCaptionBarHeight);
      arrivalCaptionShown = false;
    }
    arrivalPhase = ArrivalPhase::Idle;
    Serial.println("STATUS ARRIVAL_END");
    onArrivalRetired();
  } else {
    setBacklightNow(
        lerpBrightness(kBrightnessArrivalFull, kBrightnessResident, t));
  }
}

// ---- Generic backlight ramp (sleep/wake; reusable by other workstreams) -----
// Forward declarations for helpers defined further below in this namespace.
bool noteAuthFailure(const char* where);
JpegDownload downloadJpeg(const String& url);
bool decodeAndDrawJpeg(uint8_t* data, size_t size);
void onArrivalRetired();
void carouselShow(size_t index);
void clearStars();

void setBacklightNow(uint8_t value) {
  currentBrightness = value;
  display.setBrightness(value);
}

void startBacklightRamp(uint8_t target, uint32_t durationMs, bool easeIn) {
  blRampFrom = currentBrightness;
  blRampTo = target;
  blRampStartMs = millis();
  blRampDurationMs = durationMs ? durationMs : 1;
  blRampEaseIn = easeIn;
  blRampActive = true;
}

void updateBacklightRamp(uint32_t now) {
  if (!blRampActive) return;
  const uint32_t elapsed = now - blRampStartMs;
  if (elapsed >= blRampDurationMs) {
    blRampActive = false;
    setBacklightNow(blRampTo);
    return;
  }
  const float t = static_cast<float>(elapsed) / blRampDurationMs;
  const float p = blRampEaseIn ? t * t : easeOutQuad(t);
  setBacklightNow(static_cast<uint8_t>(
      blRampFrom + (static_cast<int>(blRampTo) - blRampFrom) * p));
}

// ---- Sleep mode --------------------------------------------------------------
void enterSleep() {
  if (sleeping) return;
  sleeping = true;
  missedWhileSleeping = 0;
  // An in-flight arrival fade must not fight the dim-to-0 ramp; retire it
  // (repainting the sticker out first so the wake frame is clean).
  if (arrivalPhase != ArrivalPhase::Idle) repaintStickerRegion();
  resetArrival(false);
  Serial.println("SLEEP ENTER");
  startBacklightRamp(0, kSleepRampMs, true);
}

void wakeFromSleep() {
  if (!sleeping) return;
  sleeping = false;
  // Photos that arrived while asleep were already rendered silently onto the
  // framebuffer, so the newest card is on screen the moment the backlight
  // comes back.
  Serial.printf("SLEEP WAKE %u\n", static_cast<unsigned>(missedWhileSleeping));
  missedWhileSleeping = 0;
  startBacklightRamp(kBrightnessBoot, kWakeRampMs, false);
}

// ---- Info bar ("polaroid chin") ----------------------------------------------
// Bar color comes from the photo itself: average a pixel row near the bottom,
// then shift its lightness ~30% (spec: derived tint, never plain black/white).
void drawInfoBar(const String& author, const String& caption,
                 const String& indexLabel) {
  static uint16_t row[800];
  display.readRect(0, kCaptionBarY - 8, 800, 1, row);
  uint32_t sumR = 0, sumG = 0, sumB = 0;
  int samples = 0;
  for (int x = 0; x < 800; x += 16) {
    const uint16_t c = row[x];
    sumR += (c >> 11) & 0x1F;
    sumG += (c >> 5) & 0x3F;
    sumB += c & 0x1F;
    ++samples;
  }
  uint8_t r = (sumR / samples) << 3;
  uint8_t g = (sumG / samples) << 2;
  uint8_t b = (sumB / samples) << 3;
  const int luminance = (r * 299 + g * 587 + b * 114) / 1000;
  if (luminance > 140) {  // bright photo edge -> darker chin
    r = r * 13 / 20;
    g = g * 13 / 20;
    b = b * 13 / 20;
  } else {  // dark edge -> lighter chin
    r += (255 - r) * 7 / 20;
    g += (255 - g) * 7 / 20;
    b += (255 - b) * 7 / 20;
  }
  const uint32_t barColor = (static_cast<uint32_t>(r) << 16) |
                            (static_cast<uint32_t>(g) << 8) | b;
  const int barLuminance = (r * 299 + g * 587 + b * 114) / 1000;
  const uint32_t textColor = barLuminance > 128 ? kInk : kBackground;

  display.startWrite();
  display.fillRect(0, kCaptionBarY, 800, kCaptionBarHeight, barColor);
  // Hairline separator: the bar color pushed a step darker.
  const uint32_t hairline = (static_cast<uint32_t>(r * 3 / 4) << 16) |
                            (static_cast<uint32_t>(g * 3 / 4) << 8) |
                            (b * 3 / 4);
  display.fillRect(0, kCaptionBarY, 800, 2, hairline);
  display.setFont(&fonts::efontCN_16);
  display.setTextColor(textColor, barColor);
  const int16_t midY = kCaptionBarY + kCaptionBarHeight / 2 + 1;
  if (caption.length()) {
    // Caption is the hero line: 32px, centered; author/index stay 16px sides.
    display.setTextSize(2);
    String text = caption;
    bool trimmed = false;
    while (text.length() && display.textWidth(text) > 500) {
      text.remove(text.length() - 1);
      trimmed = true;
    }
    if (trimmed) text += "…";
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.drawString(text, 400, midY);
    display.setTextSize(1);
  } else {
    display.setTextSize(1);
  }
  if (author.length()) {
    display.fillCircle(20, midY, 4, kPink);
    display.setTextDatum(lgfx::textdatum_t::middle_left);
    display.drawString(author, 32, midY);
  }
  if (indexLabel.length()) {
    display.setTextDatum(lgfx::textdatum_t::middle_right);
    display.drawString(indexLabel, 784, midY);
  }
  display.endWrite();
}

const char* modeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Resident: return "常驻模式";
    case DisplayMode::Slideshow: return "轮播模式";
    default: return "最新模式";
  }
}

// Mode-name sticker toast (top center, ~1.5 s, erased from the retained JPEG).
void drawModeToast() {
  display.startWrite();
  display.fillRoundRect(288, 12, 232, 48, 12, kShadow);
  display.fillRoundRect(284, 8, 232, 48, 12, kLilac);
  display.drawRoundRect(284, 8, 232, 48, 12, TFT_WHITE);
  drawStarGlyph(310, 32, 9, kStarYellow);
  display.setFont(&fonts::efontCN_16);
  display.setTextSize(1);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(kInk, kLilac);
  display.drawString(modeName(displayMode), 410, 32);
  display.endWrite();
  modeToastActive = true;
  modeToastUntil = millis() + kModeToastMs;
}

// After the arrival ritual retires the always-on slideshow resumes its cadence
// from the newly shown card. Resident mode still returns to its pinned photo.
void onArrivalRetired() {
  nextSlideAt = millis() + kSlideshowIntervalMs;
  if (displayMode != DisplayMode::Resident || residentPhotoId.length() == 0) {
    return;
  }
  for (size_t i = 0; i < feedCount; ++i) {
    if (feedPhotos[i].photoId == residentPhotoId) {
      if (i != currentPhotoIndex) carouselShow(i);
      return;
    }
  }
  // Pinned photo fell out of the feed window: stay on the newest card.
}

// ---- Carousel ---------------------------------------------------------------
// Plain render for swipe navigation (no arrival ritual while browsing).
// Cache-first: a hit decodes straight from PSRAM (fast, no network at all);
// a miss downloads once and fills the slot cache for every later visit.
bool renderFeedPhoto(size_t index) {
  FeedPhoto& photo = feedPhotos[index];
  if (photo.jpeg == nullptr) {
    setStatus("DOWNLOADING", photo.photoId);
    JpegDownload jpeg = downloadJpeg(imageUrl(photo.imageUrl));
    if (jpeg.httpCode == 401) {
      if (jpeg.data != nullptr) heap_caps_free(jpeg.data);
      noteAuthFailure("image");
      return false;
    }
    if (jpeg.data == nullptr) {
      setStatus("IMAGE ERROR", "download rejected");
      return false;
    }
    cacheFeedJpeg(index, jpeg.data, jpeg.size);
    heap_caps_free(jpeg.data);
    if (photo.jpeg == nullptr) {
      setStatus("IMAGE ERROR", "cache alloc failed");
      return false;
    }
  }
  // Browsing away retires any in-flight arrival ritual and its sticker.
  if (arrivalPhase != ArrivalPhase::Idle) repaintStickerRegion();
  resetArrival(true);
  const bool rendered = decodeAndDrawJpeg(photo.jpeg, photo.jpegSize);
  if (!rendered) {
    freeFeedJpeg(photo);
    setStatus("IMAGE ERROR", "JPEG rejected or decode failed");
    return false;
  }
  // Overlay repaint source: own copy, since retainShownJpeg frees on replace.
  uint8_t* shownCopy = static_cast<uint8_t*>(
      heap_caps_malloc(photo.jpegSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (shownCopy != nullptr) {
    memcpy(shownCopy, photo.jpeg, photo.jpegSize);
    retainShownJpeg(shownCopy, photo.jpegSize);
  }
  hasRenderedPhoto = true;
  drawInfoBar(photo.author, photo.caption,
              String(index + 1) + "/" + String(feedCount));
  setStatus("PHOTO READY", photo.author.length() ? photo.author : String("new photo"));
  return true;
}

void carouselShow(size_t index) {
  if (index >= feedCount) return;  // 到底不动: no wrap-around on manual swipes
  currentPhotoIndex = index;
  Serial.printf("CAROUSEL %u/%u %s\n", static_cast<unsigned>(index + 1),
                static_cast<unsigned>(feedCount),
                feedPhotos[index].photoId.c_str());
  // 直接画, 不碰背光 (用户明确砍掉背光过渡)。缓存命中时整个翻页就是一次
  // 解码 (~0.3s), 已在 0.5s 预算内。
  const uint32_t startedAt = millis();
  renderFeedPhoto(index);
  nextSlideAt = millis() + kSlideshowIntervalMs;
  Serial.printf("PAGE_TURN %lums\n",
                static_cast<unsigned long>(millis() - startedAt));
}

// SWIPE_LEFT = next (newer, toward index 0); SWIPE_RIGHT = previous (older).
void carouselNewer() {
  if (currentPhotoIndex > 0) carouselShow(currentPhotoIndex - 1);
}

void carouselOlder() {
  if (currentPhotoIndex + 1 < feedCount) carouselShow(currentPhotoIndex + 1);
}

// ---- Gesture consumption (carousel + sleep) ----------------------------------
// Reads gestureQueue via a private cursor (multi-consumer safe: the TAP/like
// consumer pops the same queue independently of this cursor).
void handleCarouselAndSleepGestures() {
  while (carouselGestureCursor != gestureQueueTail) {
    const QueuedGesture gesture = gestureQueue[carouselGestureCursor];
    carouselGestureCursor = (carouselGestureCursor + 1) % kGestureQueueSize;
    if (gesture.event == GestureEvent::NONE) continue;
    if (sleeping) {
      wakeFromSleep();  // any touch event wakes the panel
      continue;         // the waking touch is swallowed, not re-interpreted
    }
    switch (gesture.event) {
      case GestureEvent::SWIPE_UP:
        break;  // 最简手势集: 上滑不再息屏 (误触黑屏太像故障)
      case GestureEvent::SWIPE_LEFT:
        carouselNewer();  // carouselShow bumps nextSlideAt itself
        break;
      case GestureEvent::SWIPE_RIGHT:
        carouselOlder();
        break;
      case GestureEvent::SWIPE_DOWN:
        Serial.println("REFRESH forced feed poll");
        pollForcedByGesture = true;  // bypasses the touch-activity poll gate
        nextCloudPollAt = 0;
        break;
      default:
        break;  // TAP / LONG_PRESS belong to like/selector
    }
  }
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
  resetArrival(true);  // stop any fade and restore panel brightness
  selectorOpen = false;
  modeToastActive = false;
  feedCount = 0;
  currentPhotoIndex = 0;
  for (size_t i = 0; i < kFeedLimit; ++i) freeFeedJpeg(feedPhotos[i]);
  clearStars();
  freeShownJpeg();
  showPanelMessage("PAIRING REQUIRED", "requesting a new 6-digit code", kPink);
}

void noteRequestSuccess() { consecutiveAuthFailures = 0; }

// Returns true when the failure budget is exhausted and the binding was
// cleared. Callers treat that as "stop this poll cycle"; transient 401s just
// keep the current photo on screen and retry next cycle.
bool noteAuthFailure(const char* where) {
  if (consecutiveAuthFailures < 255) ++consecutiveAuthFailures;
  Serial.printf("AUTH 401 %s %u/%u\n", where,
                static_cast<unsigned>(consecutiveAuthFailures),
                static_cast<unsigned>(kAuthFailureLimit));
  if (consecutiveAuthFailures < kAuthFailureLimit) {
    setStatus("AUTH WARNING", "keeping binding, retrying");
    return false;
  }
  Serial.println("AUTH 401 LIMIT reached, clearing binding");
  consecutiveAuthFailures = 0;
  clearBinding();
  return true;
}

// ---- Gesture engine implementation ------------------------------------------

const char* gestureName(GestureEvent event) {
  switch (event) {
    case GestureEvent::TAP: return "TAP";
    case GestureEvent::SWIPE_LEFT: return "SWIPE_LEFT";
    case GestureEvent::SWIPE_RIGHT: return "SWIPE_RIGHT";
    case GestureEvent::SWIPE_UP: return "SWIPE_UP";
    case GestureEvent::SWIPE_DOWN: return "SWIPE_DOWN";
    case GestureEvent::LONG_PRESS: return "LONG_PRESS";
    default: return "NONE";
  }
}

void queueGesture(GestureEvent event, int16_t x, int16_t y) {
  QueuedGesture& slot = gestureQueue[gestureQueueTail];
  slot.event = event;
  slot.x = x;
  slot.y = y;
  gestureQueueTail = (gestureQueueTail + 1) % kGestureQueueSize;
  if (gestureQueueTail == gestureQueueHead) {
    // Queue full: drop the oldest event so producers never block.
    gestureQueueHead = (gestureQueueHead + 1) % kGestureQueueSize;
  }
}

// Consumer API for other loop() features (carousel, screen-off, settings).
// Returns false when no gesture is pending. TAPs are also consumed by the
// like logic below, but remain in the queue for any other listener.
bool nextGestureEvent(GestureEvent& event, int16_t& x, int16_t& y) {
  if (gestureQueueHead == gestureQueueTail) return false;
  const QueuedGesture& queued = gestureQueue[gestureQueueHead];
  event = queued.event;
  x = queued.x;
  y = queued.y;
  gestureQueueHead = (gestureQueueHead + 1) % kGestureQueueSize;
  return true;
}

// Linear blend of two RGB888 colors; ratio 0 -> a, 1 -> b.
uint32_t mixColors(uint32_t a, uint32_t b, float ratio) {
  const uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  const uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  const uint8_t r = ar + static_cast<uint8_t>((br - ar) * ratio);
  const uint8_t g = ag + static_cast<uint8_t>((bg - ag) * ratio);
  const uint8_t bl = ab + static_cast<uint8_t>((bb - ab) * ratio);
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | bl;
}

void drawStarShape(int16_t cx, int16_t cy, float outerRadius, uint32_t color) {
  const float innerRadius = outerRadius * 0.45f;
  int32_t px[10];
  int32_t py[10];
  for (int i = 0; i < 10; ++i) {
    const float angle = -PI / 2.0f + i * PI / 5.0f;
    const float radius = (i % 2 == 0) ? outerRadius : innerRadius;
    px[i] = cx + static_cast<int32_t>(cosf(angle) * radius);
    py[i] = cy + static_cast<int32_t>(sinf(angle) * radius);
  }
  // Fan-fill the 10-vertex star outline from its center.
  for (int i = 0; i < 10; ++i) {
    display.fillTriangle(cx, cy, px[i], py[i], px[(i + 1) % 10],
                         py[(i + 1) % 10], color);
  }
}

void spawnStar(int16_t x, int16_t y) {
  StarParticle& star = stars[starWriteIndex];
  starWriteIndex = (starWriteIndex + 1) % kMaxStars;
  star.active = true;
  star.x = x;
  star.y = y;
  star.startedAt = millis();
  star.lastStep = 255;
  star.savedW = 0;  // recycled slot: any old save-under content is stale
}

void restoreStarBackground(StarParticle& star) {
  if (star.savedW > 0 && star.saved != nullptr) {
    display.pushImage(star.savedX, star.savedY, star.savedW, star.savedH,
                      star.saved);
  }
  star.savedW = 0;
}

// Deactivate all particles and drop their save-under state. Called whenever
// the pixels underneath are about to be replaced wholesale (new photo,
// selector overlay), because restoring stale backups would paint ghosts.
void clearStars() {
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    stars[i].active = false;
    stars[i].savedW = 0;
  }
}

// Renders one animation step per active particle, 8 steps over the 1 s
// lifetime (ease-out rise 0 -> -40 px, scale 0.8 -> 1.4, fading tint).
// Every step first restores the pixels under the previous frame from a
// per-star PSRAM save-under buffer, so expired stars leave no trace.
void renderStars() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    StarParticle& star = stars[i];
    if (!star.active) continue;
    const uint32_t elapsed = now - star.startedAt;
    if (elapsed >= kStarLifetimeMs) {
      restoreStarBackground(star);
      star.active = false;
      continue;
    }
    const uint8_t step = elapsed / (kStarLifetimeMs / 8);
    if (step == star.lastStep) continue;  // already drew this frame
    star.lastStep = step;

    const float progress = static_cast<float>(elapsed) / kStarLifetimeMs;
    const float easeOut = 1.0f - (1.0f - progress) * (1.0f - progress);
    const float scale = 0.8f + 0.6f * easeOut;
    const int16_t cy = star.y - static_cast<int16_t>(easeOut * 40.0f);
    const float outerRadius = 7.0f * scale;

    if (star.saved == nullptr) {
      star.saved = static_cast<uint16_t*>(
          heap_caps_malloc(kStarBufPx * kStarBufPx * sizeof(uint16_t),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    display.startWrite();
    restoreStarBackground(star);
    if (star.saved != nullptr) {
      int16_t x0 = star.x - kStarBufPx / 2;
      int16_t y0 = cy - kStarBufPx / 2;
      int16_t w = kStarBufPx;
      int16_t h = kStarBufPx;
      if (x0 < 0) { w += x0; x0 = 0; }
      if (y0 < 0) { h += y0; y0 = 0; }
      if (x0 + w > 800) w = 800 - x0;
      if (y0 + h > 480) h = 480 - y0;
      if (w > 0 && h > 0) {
        display.readRect(x0, y0, w, h, star.saved);
        star.savedX = x0;
        star.savedY = y0;
        star.savedW = w;
        star.savedH = h;
      }
    }
    // Fake the opacity fade by tinting toward a dim warm gray; true alpha is
    // impossible without knowing the photo pixels underneath.
    const uint32_t color = mixColors(kStarYellow, 0x9A927C, progress * 0.8f);
    display.drawCircle(star.x, cy, static_cast<int32_t>(outerRadius + 4),
                       mixColors(kStarYellow, 0x9A927C, 0.5f + progress * 0.4f));
    drawStarShape(star.x, cy, outerRadius, color);
    display.endWrite();
  }
}

void onTap(int16_t x, int16_t y, uint32_t now) {
  spawnStar(x, y);
  if (now - lastLikeTapAt < kMultiTapGapMs && likeTapChain < 255) {
    ++likeTapChain;
  } else {
    likeTapChain = 1;
  }
  if (pendingLikeTaps < 255) ++pendingLikeTaps;
  lastLikeTapAt = now;
  Serial.printf("TAP x=%d y=%d chain=%u pending=%u\n", x, y,
                static_cast<unsigned>(likeTapChain),
                static_cast<unsigned>(pendingLikeTaps));
}

// ---- Mode selector (long press) ----------------------------------------------
// Three cards on a dark_night band (true alpha masking is not affordable on
// this panel, so the band substitutes for the spec's 40% dimmed backdrop).
void drawModeSelector() {
  display.startWrite();
  display.fillRect(0, 132, 800, 216, kNight);
  display.setFont(&fonts::efontCN_16);
  display.setTextSize(1);
  display.setTextDatum(lgfx::textdatum_t::middle_center);
  display.setTextColor(kBackground, kNight);
  display.drawString("显示模式", 400, 150);
  drawStarGlyph(340, 150, 6, kStarYellow);
  drawStarGlyph(460, 150, 6, kStarYellow);

  const char* labels[3] = {"最新", "常驻", "轮播"};
  const char* subtitles[3] = {"新照片优先", "固定这一张", "自动翻页"};
  for (int i = 0; i < 3; ++i) {
    const int16_t x = 112 + i * 200;  // 176 px cards with 24 px gaps, centered
    const int16_t cx = x + 88;
    display.fillRoundRect(x + 4, 172, 176, 152, 20, kShadow);
    display.fillRoundRect(x, 168, 176, 152, 20, kBackground);
    if (static_cast<int>(displayMode) == i) {
      for (int inset = 0; inset < 3; ++inset) {  // 3 px cream highlight border
        display.drawRoundRect(x + inset, 168 + inset, 176 - 2 * inset,
                              152 - 2 * inset, 20 - inset, kStarYellow);
      }
    }
    // Icon glyphs, drawn from primitives (no icon font on board).
    const int16_t cy = 212;
    if (i == 0) {  // Latest: cream six-point star
      drawStarGlyph(cx, cy, 18, kStarYellow);
      display.fillCircle(cx, cy, 5, kBackground);
    } else if (i == 1) {  // Resident: candy-pink pin
      display.fillCircle(cx, cy - 4, 13, kPink);
      display.fillTriangle(cx - 9, cy + 4, cx + 9, cy + 4, cx, cy + 22, kPink);
      display.fillCircle(cx, cy - 4, 5, kBackground);
    } else {  // Slideshow: two stacked photo cards
      display.fillRoundRect(cx - 20, cy - 16, 28, 22, 4, kLilac);
      display.fillRoundRect(cx - 8, cy - 4, 28, 22, 4, kBlue);
    }
    display.setTextSize(2);
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(kInk, kBackground);
    display.drawString(labels[i], cx, 262);
    display.setTextSize(1);
    display.setTextColor(kMuted, kBackground);
    display.drawString(subtitles[i], cx, 300);
  }
  display.endWrite();
}

void openSelector() {
  if (selectorOpen || shownJpeg == nullptr) return;  // need a repaint source
  clearStars();
  selectorOpen = true;
  selectorOpenedAt = millis();
  drawModeSelector();
  Serial.println("SELECTOR OPEN");
}

void closeSelector(bool withToast) {
  if (!selectorOpen) return;
  selectorOpen = false;
  repaintFullFromShown();
  Serial.printf("SELECTOR CLOSE mode=%d\n", static_cast<int>(displayMode));
  if (withToast) drawModeToast();
}

void selectorHandleTap(int16_t x, int16_t y) {
  if (y >= 168 && y < 320) {
    for (int i = 0; i < 3; ++i) {
      const int16_t cardX = 112 + i * 200;
      if (x >= cardX && x < cardX + 176) {
        displayMode = static_cast<DisplayMode>(i);
        if (displayMode == DisplayMode::Resident) {
          residentPhotoId = feedCount > 0
                                ? feedPhotos[currentPhotoIndex].photoId
                                : latestPhotoId;
        } else if (displayMode == DisplayMode::Slideshow) {
          nextSlideAt = millis() + kSlideshowIntervalMs;
        }
        Serial.printf("MODE %s\n", modeName(displayMode));
        closeSelector(true);
        return;
      }
    }
  }
  closeSelector(false);  // tap outside the cards cancels
}

void pollTouchGestures() {
  const uint32_t now = millis();
  uint16_t rawX = 0, rawY = 0;
  const bool touching = display.getTouch(&rawX, &rawY);

  if (touching) {
    const int16_t x = static_cast<int16_t>(rawX);
    const int16_t y = static_cast<int16_t>(rawY);
    if (!touchHeld) {
      // IDLE -> TOUCH_DOWN: remember origin and timestamp.
      touchHeld = true;
      longPressFired = false;
      touchDownX = touchLastX = x;
      touchDownY = touchLastY = y;
      touchDownAt = now;
      lastUserTouchAt = now;  // gates polling + auto-slideshow
      return;
    }
    touchLastX = x;
    touchLastY = y;
    if (!longPressFired && now - touchDownAt >= kLongPressMs &&
        abs(x - touchDownX) < kTapMaxMovePx &&
        abs(y - touchDownY) < kTapMaxMovePx) {
      longPressFired = true;  // 最简手势集: 长按吞掉不做事 (模式盘已砍)
      Serial.printf("GESTURE LONG_PRESS x=%d y=%d (ignored)\n", touchDownX,
                    touchDownY);
    }
    return;
  }

  if (!touchHeld) return;  // still IDLE
  touchHeld = false;
  if (longPressFired) return;  // release after long press: no tap/swipe

  const uint32_t duration = now - touchDownAt;
  const int dx = touchLastX - touchDownX;
  const int dy = touchLastY - touchDownY;
  const int adx = abs(dx);
  const int ady = abs(dy);

  if (adx < kTapMaxMovePx && ady < kTapMaxMovePx && duration < kTapMaxMs) {
    if (sleeping) {
      // A waking tap only wakes: no star, no like.
      queueGesture(GestureEvent::TAP, touchLastX, touchLastY);
    } else if (selectorOpen) {
      selectorHandleTap(touchLastX, touchLastY);
    } else {
      queueGesture(GestureEvent::TAP, touchLastX, touchLastY);
      onTap(touchLastX, touchLastY, now);
    }
  } else if ((adx > kSwipeMinMovePx || ady > kSwipeMinMovePx) &&
             duration < kSwipeMaxMs) {
    // GT911 on this panel reports X mirrored against the RGB framebuffer, so
    // the horizontal direction is flipped to match what the user sees.
    const GestureEvent event =
        adx > ady ? (dx > 0 ? GestureEvent::SWIPE_LEFT : GestureEvent::SWIPE_RIGHT)
                  : (dy > 0 ? GestureEvent::SWIPE_DOWN : GestureEvent::SWIPE_UP);
    Serial.printf("GESTURE %s x=%d y=%d\n", gestureName(event), touchLastX,
                  touchLastY);
    if (selectorOpen && !sleeping) {
      closeSelector(false);  // any swipe dismisses the selector
    } else {
      queueGesture(event, touchLastX, touchLastY);
    }
  }
  // Slow drags outside both envelopes are deliberately ignored.
}

// Aggregated like reporting: taps accumulate, then 2 s of silence triggers one
// POST /v1/reactions {"photo_id": ..., "tap_count": N}. 401 feeds the shared
// auth-failure budget; a network error (-1) gets one silent retry, then the
// batch is dropped (likes are best-effort).
void reportPendingLikes() {
  if (pendingLikeTaps == 0) return;
  if (millis() - lastLikeTapAt < kLikeWindowMs) return;
  // Like the photo the user is actually looking at, not the newest in feed.
  const String& likedPhotoId =
      (feedCount > 0) ? feedPhotos[currentPhotoIndex].photoId : latestPhotoId;
  if (token.length() == 0 || likedPhotoId.length() == 0) {
    pendingLikeTaps = 0;
    return;
  }

  const uint8_t taps = pendingLikeTaps;
  Serial.printf("LIKE %s x%u\n", likedPhotoId.c_str(),
                static_cast<unsigned>(taps));
  JsonDocument body;
  body["photo_id"] = likedPhotoId;
  body["tap_count"] = taps;
  String payload;
  serializeJson(body, payload);

  HttpResponse response = requestJson("POST", "/v1/reactions", payload, true);
  if (response.code == -1) {
    response = requestJson("POST", "/v1/reactions", payload, true);
  }
  if (response.code == 401) {
    noteAuthFailure("reactions");
    Serial.printf("LIKE RESULT %s x%u 401\n", likedPhotoId.c_str(),
                  static_cast<unsigned>(taps));
  } else if (response.code >= 200 && response.code < 300) {
    noteRequestSuccess();
    Serial.printf("LIKE RESULT %s x%u OK %d\n", likedPhotoId.c_str(),
                  static_cast<unsigned>(taps), response.code);
  } else {
    Serial.printf("LIKE RESULT %s x%u FAILED %d\n", likedPhotoId.c_str(),
                  static_cast<unsigned>(taps), response.code);
  }
  pendingLikeTaps = 0;
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
  consecutiveAuthFailures = 0;
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

  // Cover-fit constants (kCoverScale / kScaledVerticalCrop) live at namespace
  // scope so the arrival ritual can repaint the sticker region identically.
  clearStars();  // star save-under state is stale once the photo is replaced
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
    noteAuthFailure("feed");
    return;
  }
  noteRequestSuccess();
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

  // Cache metadata for up to kFeedLimit newest photos. JPEG caches migrate by
  // photoId so a feed shift (new arrival pushes everything down one slot)
  // doesn't throw away bytes we already downloaded.
  uint8_t* oldJpeg[kFeedLimit];
  size_t oldJpegSize[kFeedLimit];
  String oldId[kFeedLimit];
  const size_t oldCount = feedCount;
  for (size_t i = 0; i < oldCount; ++i) {
    oldId[i] = feedPhotos[i].photoId;
    oldJpeg[i] = feedPhotos[i].jpeg;
    oldJpegSize[i] = feedPhotos[i].jpegSize;
    feedPhotos[i].jpeg = nullptr;
    feedPhotos[i].jpegSize = 0;
  }
  size_t count = 0;
  for (JsonObject entry : items) {
    if (count >= kFeedLimit) break;
    const char* entryId = entry["photo_id"] | "";
    const char* entryUrl = entry["image_url"] | "";
    if (strlen(entryId) == 0 || strlen(entryUrl) == 0) continue;
    FeedPhoto& slot = feedPhotos[count++];
    slot.photoId = entryId;
    slot.imageUrl = entryUrl;
    slot.author = entry["author"]["display_name"] | "";
    slot.caption = entry["caption"] | "";
    for (size_t i = 0; i < oldCount; ++i) {
      if (oldJpeg[i] != nullptr && oldId[i] == slot.photoId) {
        slot.jpeg = oldJpeg[i];
        slot.jpegSize = oldJpegSize[i];
        oldJpeg[i] = nullptr;
        break;
      }
    }
  }
  for (size_t i = 0; i < oldCount; ++i) {
    if (oldJpeg[i] != nullptr) heap_caps_free(oldJpeg[i]);
  }
  if (count == 0) {
    setStatus("FEED INVALID", "missing photo_id/image_url");
    return;
  }
  feedCount = count;
  if (currentPhotoIndex >= feedCount) currentPhotoIndex = feedCount - 1;

  const FeedPhoto& newest = feedPhotos[0];
  if (latestPhotoId == newest.photoId && hasRenderedPhoto) {
    setStatus("ONLINE", "latest photo is current");
    return;
  }

  // New arrival: the carousel jumps back to the newest card.
  currentPhotoIndex = 0;
  setStatus("DOWNLOADING", newest.photoId);
  JpegDownload jpeg = downloadJpeg(imageUrl(newest.imageUrl));
  if (jpeg.httpCode == 401) {
    if (jpeg.data != nullptr) heap_caps_free(jpeg.data);
    noteAuthFailure("image");
    return;
  }
  if (jpeg.data == nullptr) {
    setStatus("IMAGE ERROR", "download rejected");
    return;
  }
  // Slot 0 cache: later swipes back to the newest photo skip the network.
  cacheFeedJpeg(0, jpeg.data, jpeg.size);

  if (sleeping) {
    // Silent ingest while the panel is off: draw the pixels (backlight stays
    // 0 so nothing is visible), skip the arrival ritual entirely, and count
    // the photo as missed for the SLEEP WAKE log.
    if (arrivalPhase != ArrivalPhase::Idle) repaintStickerRegion();
    resetArrival(false);
    const bool rendered = decodeAndDrawJpeg(jpeg.data, jpeg.size);
    if (!rendered) {
      heap_caps_free(jpeg.data);
      setStatus("IMAGE ERROR", "JPEG rejected or decode failed");
      return;
    }
    retainShownJpeg(jpeg.data, jpeg.size);
    latestPhotoId = newest.photoId;
    preferences.putString("latest_photo", latestPhotoId);
    hasRenderedPhoto = true;
    if (missedWhileSleeping < 255) ++missedWhileSleeping;
    Serial.printf("CAROUSEL 1/%u %s\n", static_cast<unsigned>(feedCount),
                  newest.photoId.c_str());
    setStatus("PHOTO READY",
              newest.author.length() ? newest.author : String("new photo"));
    return;
  }

  // Arrival ritual: drop the backlight before the new pixels land, then the
  // fade-in rides the PWM (an alpha blend would blow the PSRAM bandwidth).
  selectorOpen = false;      // the arrival takes over the whole screen
  modeToastActive = false;
  resetArrival(false);  // retire any previous ritual
  setBacklightNow(kBrightnessArrivalLow);
  const bool rendered = decodeAndDrawJpeg(jpeg.data, jpeg.size);
  if (!rendered) {
    heap_caps_free(jpeg.data);
    setBacklightNow(kBrightnessBoot);
    setStatus("IMAGE ERROR", "JPEG rejected or decode failed");
    return;
  }

  latestPhotoId = newest.photoId;
  preferences.putString("latest_photo", latestPhotoId);
  hasRenderedPhoto = true;
  const char* author =
      newest.author.length() ? newest.author.c_str() : "new photo";
  // Keep the JPEG in PSRAM so the sticker and caption bar can be painted
  // back out of the photo when the ritual retires.
  retainShownJpeg(jpeg.data, jpeg.size);
  drawNameSticker(author);
  if (newest.caption.length()) {
    drawInfoBar(String(), newest.caption, String());
    arrivalCaptionShown = true;
  }
  beginArrival(newest.photoId.c_str(), author);
  Serial.printf("CAROUSEL 1/%u %s\n", static_cast<unsigned>(feedCount),
                newest.photoId.c_str());
  setStatus("PHOTO READY", author);
}

void pollCloud() {
  const HttpResponse state = requestJson("GET", "/device/state");
  if (state.code == 401) {
    if (noteAuthFailure("state")) return;  // budget exhausted, re-pairing
  } else if (state.code > 0) {
    noteRequestSuccess();
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
  processFeed(requestJson("GET", "/feed?limit=" + String(kFeedLimit)));
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
  setBacklightNow(kBrightnessBoot);
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
  // Keep the arrival backlight animation running alongside network polling.
  updateArrival(now);
  // Sleep/wake backlight ramp (idle unless a transition is in flight).
  updateBacklightRamp(now);
  // Carousel paging + sleep/wake consume the gesture queue via a private
  // cursor; TAP events remain available to the like consumer.
  handleCarouselAndSleepGestures();
  // Mode selector timeout, toast expiry, and slideshow auto-advance.
  if (selectorOpen && timeReached(now, selectorOpenedAt + kSelectorTimeoutMs)) {
    closeSelector(false);
  }
  if (modeToastActive && timeReached(now, modeToastUntil)) {
    modeToastActive = false;
    repaintRegionFromShown(280, 4, 244, 60);
  }
  // Always-on slideshow: 5 s cadence whenever nobody is touching the panel
  // (15 s hold after the last touch) and no ritual is in flight.
  if (!selectorOpen && !sleeping && arrivalPhase == ArrivalPhase::Idle &&
      feedCount > 1 && hasRenderedPhoto && timeReached(now, nextSlideAt) &&
      timeReached(now, lastUserTouchAt + kAutoSlideTouchHoldMs)) {
    carouselShow((currentPhotoIndex + 1) % feedCount);  // auto mode wraps
  }
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
  } else if (timeReached(now, nextCloudPollAt) &&
             (pollForcedByGesture ||
              timeReached(now, lastUserTouchAt + kPollTouchHoldMs))) {
    // Touch gate: a synchronous poll blocks this loop for seconds on a slow
    // hotspot, which reads as "the frame is frozen". While fingers are active
    // we hold the poll; SWIPE_DOWN's forced refresh goes through regardless.
    pollForcedByGesture = false;
    nextCloudPollAt = now + kCloudPollMs;
    pollCloud();
  } else if (token.length() > 0 && feedCount > 0 && !sleeping &&
             arrivalPhase == ArrivalPhase::Idle &&
             timeReached(now, nextPrefetchAt) &&
             timeReached(now, lastUserTouchAt + kPrefetchIdleMs)) {
    // Idle prefetch: pull one uncached feed JPEG per pass so later swipes are
    // pure decode (no network on the page-turn path).
    for (size_t i = 0; i < feedCount; ++i) {
      if (feedPhotos[i].jpeg != nullptr) continue;
      JpegDownload jpeg = downloadJpeg(imageUrl(feedPhotos[i].imageUrl));
      if (jpeg.data != nullptr) {
        cacheFeedJpeg(i, jpeg.data, jpeg.size);
        heap_caps_free(jpeg.data);
        Serial.printf("PREFETCH %u/%u %s cached\n", static_cast<unsigned>(i + 1),
                      static_cast<unsigned>(feedCount),
                      feedPhotos[i].photoId.c_str());
        nextPrefetchAt = millis() + 250;
      } else {
        nextPrefetchAt = millis() + kPrefetchRetryMs;
      }
      break;
    }
  }

  // Touch gestures + like stars + aggregated like reporting. Non-blocking;
  // the gesture engine lives in the anonymous namespace above.
  pollTouchGestures();
  if (!selectorOpen) renderStars();
  reportPendingLikes();

  delay(25);
}
