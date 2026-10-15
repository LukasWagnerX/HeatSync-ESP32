// Arduino IDE: select a partition scheme with at least 2 MB app space.
// Recommended: Tools > Partition Scheme > Huge APP (3MB No OTA/1MB SPIFFS).
// The menu catalog uses SPIFFS; WiFi and setup remain in NVS.
// The default 1.31 MB OTA app partition is too small.

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <FS.h>
#include <SPIFFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>
#include <ArduinoJson.h>
#include <BLESecurity.h>
#include <atomic>
#include <nvs_flash.h>
#include <esp_rom_crc.h>
#include "FactoryResetButton.h"
#include "StatusLED.h"
#include "LoopHealth.h"
#include "ReadingScheduler.h"

static const char *SETUP_AP_NAME = "HeatSync-Setup";
static const char *SETUP_AP_PASSWORD = "setup-not-active";
static const char *HEATSYNC_UPLOAD_FILE = "/eta_upload.json";
static const char *BLE_DEVICE_NAME = "HeatSync ESP32";
static const char *BLE_SERVICE_UUID = "4ba918a0-7d6f-4f8f-9d71-0f6f7d7f0101";
static const char *BLE_WRITE_UUID = "4ba918a1-7d6f-4f8f-9d71-0f6f7d7f0101";
static const char *BLE_STATUS_UUID = "4ba918a2-7d6f-4f8f-9d71-0f6f7d7f0101";

static const uint32_t POLL_INTERVAL_MS = 500;
static const uint32_t HEATSYNC_REQUEST_GAP_MS = 350;
static const size_t HEATSYNC_VALUES_PER_BATCH = 4;
static const size_t PRIORITY_VALUES_PER_BATCH = 3;
static const uint32_t WIFI_RETRY_MS = 10000;
static const uint32_t PRIORITY_REFRESH_MS = 15000;
static const bool HEATSYNC_TRACE_REQUESTS = false;
static const uint8_t SETUP_BUTTON_PIN = 0;
// Classic ESP32 DevKit indicator, not the permanently wired power LED.
static const uint8_t STATUS_LED_PIN = 2;
static const bool STATUS_LED_ACTIVE_HIGH = true;
static std::atomic<StatusLEDMode> statusLEDMode{StatusLEDMode::off};
static std::atomic<bool> bleStatusAfterAuthentication{false};
static uint32_t setupDeadline = 0;
static uint32_t lastOwnerResetCheck = 0;
static std::atomic<bool> physicalFactoryResetRequested{false};
static std::atomic<bool> runtimeWatchdogArmed{false};
static std::atomic<uint32_t> runtimeProgressAt{0};
static std::atomic<uint32_t> runtimePhase{0};
RTC_NOINIT_ATTR static uint32_t recoveryMagic;
RTC_NOINIT_ATTR static uint32_t lastRecoveryPhase;
static uint32_t lastFactoryResetSync = 0;

struct HeatSyncPoint {
  char uri[40];
  char name[96];
  char group[64];
  char path[256];
};

static const char *HEATSYNC_POINTS_FILE = "/eta_points.bin";

struct HeatSyncValue {
  String uri;
  String name;
  String group;
  String strValue;
  String raw;
  String unit;
  String decPlaces;
  String scaleFactor;
  String advTextOffset;
};

struct DiscoveredDevice {
  String label;
  String url;
};

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

String wifiSsid;
String wifiPass;
String etaBaseUrl;
String dashboardPassword;
bool configured = false;
bool setupMode = false;
bool restartRequested = false;
uint32_t lastPoll = 0;
uint32_t lastWifiRetry = 0;
uint32_t lastDiscovery = 0;
uint32_t lastPriorityFetch = 0;
uint32_t restartAt = 0;
uint32_t lastBleStatus = 0;
uint32_t lastUploadAt = 0;
int lastUploadCode = 0;
size_t lastValueCount = 0;
String lastError;
String ownerResetID;
String factoryResetID;
size_t etaPointCount = 0;
size_t nextPointIndex = 0;
bool catalogPending = true;
size_t nextPriorityIndex = 0;
size_t nextFocusIndex = 0;
size_t nextVisibleIndex = 0;
size_t nextPersistentTurn = 0;
std::vector<String> visiblePriorityUris;
std::vector<uint32_t> pointURIHashes;
std::vector<uint32_t> pointReadAt;
std::vector<String> criticalPriorityUris;
std::vector<String> instantPriorityUris;
std::vector<String> persistentReadingUris;
uint32_t lastETARequestCompleted = 0;
size_t catalogUploadIndex = 0;
uint32_t readingBatchNumber = 0;

class HeatSyncUploadBuffer : public Print {
public:
  String body;
  HeatSyncUploadBuffer() { body.reserve(12288); }
  size_t write(uint8_t value) override { return body.concat((char) value) ? 1 : 0; }
  size_t write(const uint8_t *data, size_t size) override { return body.concat((const char *) data, size) ? size : 0; }
};
std::vector<String> focusPriorityUris;
std::vector<String> priorityUris;
std::vector<DiscoveredDevice> discoveredDevices;
uint8_t deviceScanNextHost = 1;
bool deviceScanRunning = false;
BLECharacteristic *bleStatusCharacteristic = nullptr;
BLEServer *bleServerInstance = nullptr;
String pendingBleCommand;
String pendingControlRefreshURI;
bool hasPendingBleCommand = false;
QueueHandle_t bleCommands = nullptr;
std::atomic<bool> bleAuthenticated{false};
String wifiNetworksJson = "[]";
bool bleActive = false;
uint32_t bleReleaseAt = 0;
uint32_t lastBleInteraction = 0;

void notifyBleStatus(String message = "");
void releaseBleAfterSetup();
bool applyReadingPriorities(const String &body);
#include "CloudTransport.h"

void setLastError(const String &message) {
  lastError = message;
  if (Serial.availableForWrite() >= message.length() + 12) { Serial.print("HeatSync: "); Serial.println(message); }
}

void copyHeatSyncText(char *destination, size_t capacity, const String &value) {
  if (capacity == 0) return;
  size_t length = value.length() < capacity ? value.length() : capacity - 1;
  if (length < value.length()) {
    while (length > 0 && (((uint8_t) value[length]) & 0xC0) == 0x80) --length;
  }
  memcpy(destination, value.c_str(), length);
  destination[length] = 0;
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  return value;
}

String jsonEscape(String value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String urlDecode(String value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '+' ) {
      out += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char hex[3] = { value[i + 1], value[i + 2], 0 };
      out += (char) strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String xmlUnescape(String value) {
  value.replace("&quot;", "\"");
  value.replace("&apos;", "'");
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&amp;", "&");
  return value;
}

String xmlAttr(const String &tag, const String &name) {
  String key = name + "=\"";
  int start = tag.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  int end = tag.indexOf("\"", start);
  if (end < 0) return "";
  return xmlUnescape(tag.substring(start, end));
}

String normalizeUri(String uri) {
  uri.trim();
  if (uri.startsWith("/user/var/")) uri = uri.substring(9);
  if (!uri.startsWith("/")) uri = "/" + uri;
  return uri;
}

int uriDepth(const String &uri) {
  int count = 0;
  for (size_t i = 0; i < uri.length(); i++) {
    if (uri[i] == '/') count++;
  }
  return count;
}

String normalizeBaseUrl(String value) {
  value.trim();
  if (!value.startsWith("http://") && !value.startsWith("https://")) {
    value = "http://" + value;
  }
  int schemeEnd = value.indexOf("://");
  int hostStart = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  int pathStart = value.indexOf("/", hostStart);
  String hostPort = pathStart >= 0 ? value.substring(hostStart, pathStart) : value.substring(hostStart);
  if (hostPort.indexOf(":") < 0) {
    if (pathStart >= 0) {
      value = value.substring(0, pathStart) + ":8080" + value.substring(pathStart);
    } else {
      value += ":8080";
    }
  }
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

HeatSyncPoint findPoint(String uri) {
  uri = normalizeUri(uri);
  HeatSyncPoint fallback{};
  copyHeatSyncText(fallback.uri, sizeof(fallback.uri), uri);
  return fallback;
}

void loadConfig() {
  prefs.begin("heatsync", false);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  etaBaseUrl = prefs.getString("eta", "");
  dashboardPassword = prefs.getString("dashpass", "");
  serviceURL = prefs.getString("service", ""); deviceToken = prefs.getString("token", ""); heaterID = prefs.getString("heater", "");
  configured = prefs.getBool("configured", false) && !serviceURL.isEmpty() && deviceToken.length() == 64;
}

void saveConfig() {
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("eta", etaBaseUrl);
  prefs.putString("dashpass", dashboardPassword);
  prefs.putBool("configured", configured);
}

bool connectWifi(uint32_t timeoutMs = 20000) {
  if (wifiSsid.length() == 0) return false;
  Serial.print("WLAN wird verbunden: ");
  Serial.println(wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) {
    delay(250);
    if (setupMode) {
      dnsServer.processNextRequest();
      server.handleClient();
    }
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    Serial.print("WLAN verbunden, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    setLastError("WLAN Verbindung fehlgeschlagen");
  }
  return connected;
}

String httpGetPlain(const String &url, uint32_t timeoutMs = 8000) {
  const uint32_t elapsed = millis() - lastETARequestCompleted;
  if (lastETARequestCompleted && elapsed < HEATSYNC_REQUEST_GAP_MS) delay(HEATSYNC_REQUEST_GAP_MS - elapsed);
  if (!url.startsWith("http://")) {
    setLastError("ETA muss ueber HTTP erreichbar sein");
    return "";
  }

  String authorityAndPath = url.substring(7);
  int pathStart = authorityAndPath.indexOf('/');
  String authority = pathStart >= 0 ? authorityAndPath.substring(0, pathStart) : authorityAndPath;
  String path = pathStart >= 0 ? authorityAndPath.substring(pathStart) : "/";
  int port = 80;
  int portSeparator = authority.lastIndexOf(':');
  if (portSeparator > 0) {
    port = authority.substring(portSeparator + 1).toInt();
    authority = authority.substring(0, portSeparator);
  }

  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(authority.c_str(), port, 3000)) {
    setLastError("ETA HTTP Verbindung fehlgeschlagen");
    return "";
  }

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(authority);
  client.print("\r\nConnection: close\r\nAccept: application/xml\r\n\r\n");

  uint32_t deadline = millis() + timeoutMs;
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int firstSpace = statusLine.indexOf(' ');
  int secondSpace = firstSpace >= 0 ? statusLine.indexOf(' ', firstSpace + 1) : -1;
  int code = secondSpace > firstSpace ? statusLine.substring(firstSpace + 1, secondSpace).toInt() : -1;
  int contentLength = -1;
  while (millis() < deadline && client.connected()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) break;
    String lower = header;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = lower.substring(15).toInt();
  }

  String body;
  if (contentLength > 0 && contentLength < 300000) body.reserve(contentLength + 1);
  while (millis() < deadline && (client.connected() || client.available())) {
    while (client.available()) {
      char buffer[256];
      size_t availableBytes = client.available();
      size_t wanted = availableBytes < sizeof(buffer) ? availableBytes : sizeof(buffer);
      size_t readBytes = client.readBytes(buffer, wanted);
      if (readBytes > 0) body.concat(buffer, readBytes);
    }
    if (contentLength > 0 && (int) body.length() >= contentLength) break;
    if (!client.connected() && !client.available()) break;
    delay(1);
  }
  client.stop();
  lastETARequestCompleted = millis();

  if (HEATSYNC_TRACE_REQUESTS) Serial.printf("HTTP GET %s -> %d, bytes=%u\n", path.c_str(), code, (unsigned) body.length());
  if (code < 200 || code >= 300) setLastError("HTTP GET Fehler " + String(code));
  return body;
}

bool openHeatSyncGet(const String &url, WiFiClient &client, int &code, uint32_t timeoutMs = 12000) {
  if (!url.startsWith("http://")) {
    setLastError("ETA muss ueber HTTP erreichbar sein");
    return false;
  }

  String authorityAndPath = url.substring(7);
  int pathStart = authorityAndPath.indexOf('/');
  String authority = pathStart >= 0 ? authorityAndPath.substring(0, pathStart) : authorityAndPath;
  String path = pathStart >= 0 ? authorityAndPath.substring(pathStart) : "/";
  int port = 80;
  int portSeparator = authority.lastIndexOf(':');
  if (portSeparator > 0) {
    port = authority.substring(portSeparator + 1).toInt();
    authority = authority.substring(0, portSeparator);
  }

  client.setTimeout(1000);
  if (!client.connect(authority.c_str(), port, 3000)) {
    setLastError("ETA HTTP Verbindung fehlgeschlagen");
    return false;
  }
  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(authority);
  client.print("\r\nConnection: close\r\nAccept: application/xml\r\n\r\n");

  uint32_t deadline = millis() + timeoutMs;
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int firstSpace = statusLine.indexOf(' ');
  int secondSpace = firstSpace >= 0 ? statusLine.indexOf(' ', firstSpace + 1) : -1;
  code = secondSpace > firstSpace ? statusLine.substring(firstSpace + 1, secondSpace).toInt() : -1;
  while (millis() < deadline && client.connected()) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (header.length() == 0) break;
  }
  if (code < 200 || code >= 300) setLastError("HTTP GET Fehler " + String(code));
  return true;
}

bool etaLooksAlive(String baseUrl) {
  String body = httpGetPlain(normalizeBaseUrl(baseUrl) + "/user/api", 2500);
  body.toLowerCase();
  return body.indexOf("<api") >= 0 || body.indexOf("eta") >= 0;
}

void discoverMenu() {
  etaPointCount = 0;
  pointURIHashes.clear();
  SPIFFS.remove(HEATSYNC_POINTS_FILE);
  File pointsFile = SPIFFS.open(HEATSYNC_POINTS_FILE, FILE_WRITE);
  if (!pointsFile) {
    setLastError("ETA-Punktdatei konnte nicht geoeffnet werden");
    return;
  }
  Serial.print("ETA-Menue wird gelesen: ");
  Serial.println(etaBaseUrl);
  WiFiClient client;
  int code = -1;
  if (!openHeatSyncGet(etaBaseUrl + "/user/menu", client, code, 15000)) {
    pointsFile.close();
    return;
  }

  String group = "Heizung";
  String objectPath[16];
  uint8_t objectDepth = 0;
  String tag;
  tag.reserve(220);
  uint32_t deadline = millis() + 15000;
  size_t bytes = 0;
  while (millis() < deadline && (client.connected() || client.available())) {
    while (client.available()) {
      char c = (char) client.read();
      bytes++;
      if (c == '<') {
        tag = "<";
      } else if (tag.length() > 0) {
        tag += c;
        if (c == '>') {
          if (tag.startsWith("<fub ")) {
            String name = xmlAttr(tag, "name");
            if (name.length() > 0) group = name;
            objectDepth = 0;
          } else if (tag.startsWith("</fub")) {
            group = "Heizung";
            objectDepth = 0;
          } else if (tag.startsWith("<object ")) {
            String uri = normalizeUri(xmlAttr(tag, "uri"));
            String name = xmlAttr(tag, "name");
            uint8_t currentDepth = objectDepth < 16 ? objectDepth + 1 : 16;
            if (currentDepth > 0) objectPath[currentDepth - 1] = name;
            if (uriDepth(uri) == 5 && name.length() > 0) {
              HeatSyncPoint point{};
              String path = group;
              for (uint8_t i = 0; i < currentDepth; i++) {
                if (objectPath[i].length() == 0) continue;
                path += " > ";
                path += objectPath[i];
              }
              copyHeatSyncText(point.uri, sizeof(point.uri), uri);
              copyHeatSyncText(point.name, sizeof(point.name), name);
              copyHeatSyncText(point.group, sizeof(point.group), group);
              copyHeatSyncText(point.path, sizeof(point.path), path);
              if (pointsFile.write((const uint8_t *) &point, sizeof(point)) == sizeof(point)) {
                etaPointCount++;
                pointURIHashes.push_back(esp_rom_crc32_le(0, (const uint8_t *) point.uri, strlen(point.uri)));
              }
            }
            if (!tag.endsWith("/>") && currentDepth < 16) objectDepth = currentDepth;
          } else if (tag.startsWith("</object")) {
            if (objectDepth > 0) objectDepth--;
          }
          tag = "";
        }
      }
    }
    delay(1);
  }
  client.stop();
  pointsFile.close();
  catalogPending = true;
  catalogUploadIndex = 0;
  pointReadAt.assign(etaPointCount, 0);
  Serial.print("ETA-Menue Bytes: ");
  Serial.println(bytes);
  Serial.print("ETA-Punkte erkannt: ");
  Serial.println(etaPointCount);
  if (etaPointCount == 0) setLastError("Keine ETA-Werte im Menue erkannt");
}

size_t writeUploadValues(const String &xml, Print &file, size_t valueCount, const char *pointName, const char *pointGroup, const char *pointPath) {
  int pos = 0;
  while (true) {
    int variableStart = xml.indexOf("<variable ", pos);
    int valueStart = xml.indexOf("<value ", pos);
    int start = -1;
    String closeTag;
    if (variableStart >= 0 && (valueStart < 0 || variableStart < valueStart)) {
      start = variableStart;
      closeTag = "</variable>";
    } else if (valueStart >= 0) {
      start = valueStart;
      closeTag = "</value>";
    }
    if (start < 0) break;
    int tagEnd = xml.indexOf(">", start);
    if (tagEnd < 0) break;
    int close = xml.indexOf(closeTag, tagEnd);
    if (close < 0) break;
    String tag = xml.substring(start, tagEnd + 1);
    String uri = normalizeUri(xmlAttr(tag, "uri"));
    String strValue = xmlAttr(tag, "strValue");
    String unit = xmlAttr(tag, "unit");
    String decPlaces = xmlAttr(tag, "decPlaces");
    String scaleFactor = xmlAttr(tag, "scaleFactor");
    String advTextOffset = xmlAttr(tag, "advTextOffset");
    String raw = xmlUnescape(xml.substring(tagEnd + 1, close));
    raw.trim();

    String displayName = pointName && pointName[0] ? String(pointName) : uri;
    String displayGroup = pointGroup && pointGroup[0] ? String(pointGroup) : "Heizung";
    String displayPath = pointPath && pointPath[0] ? String(pointPath) : displayGroup + " > " + displayName;
    if (valueCount > 0) file.print(",");
    JsonDocument valueJSON;
    valueJSON["uri"] = uri; valueJSON["name"] = displayName;
    valueJSON["group"] = displayGroup; valueJSON["path"] = displayPath;
    valueJSON["strValue"] = strValue; valueJSON["raw"] = raw; valueJSON["unit"] = unit;
    if (decPlaces.length()) valueJSON["decPlaces"] = decPlaces.toInt();
    if (scaleFactor.length()) valueJSON["scaleFactor"] = scaleFactor.toFloat();
    if (advTextOffset.length()) valueJSON["advTextOffset"] = advTextOffset.toInt();
    serializeJson(valueJSON, file);
    valueCount++;
    pos = close + closeTag.length();
  }
  return valueCount;
}

bool uploadToWeb(const String &body) {
  String response; int code = cloudRequest("/ingest.php", body, response);
  lastUploadCode = code;
  if (code >= 200 && code < 300) {
    applyReadingPriorities(response);
    lastUploadAt = millis(); lastError = ""; return true;
  }
  setLastError("Cloud-Verbindung fehlgeschlagen: " + String(code)); return false;
}

bool containsUri(const std::vector<String> &uris, const String &candidate) {
  for (const String &uri : uris) {
    if (uri == candidate) return true;
  }
  return false;
}

bool sameUriList(const std::vector<String> &left, const std::vector<String> &right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); i++) {
    if (left[i] != right[i]) return false;
  }
  return true;
}

bool fetchPrioritiesFromWeb() {
  String body; int code = cloudRequest("/priorities.php", "", body);
  return code == 200 && applyReadingPriorities(body);
}

bool applyReadingPriorities(const String &body) {
  if (body.length() > 32000) return false;
  JsonDocument document;
  if (deserializeJson(document, body)) return false;
  JsonObject root = document["priorities"].is<JsonObject>() ? document["priorities"].as<JsonObject>() : document.as<JsonObject>();
  if (!root["uris"].is<JsonArray>()) return false;
  auto parseUriArray = [&](const char *keyName, std::vector<String> &out) {
    for (JsonVariant value : root[keyName].as<JsonArray>()) {
      if (!value.is<const char *>() || out.size() >= 64) continue;
      String uri = normalizeUri(value.as<String>());
      bool valid = uriDepth(uri) == 5 && uri.length() < sizeof(HeatSyncPoint::uri);
      for (size_t index = 0; index < uri.length(); ++index) {
        const char character = uri[index];
        if (character != '/' && (character < '0' || character > '9')) valid = false;
      }
      if (valid && !containsUri(out, uri)) out.push_back(uri);
    }
  };

  std::vector<String> fetchedFocus;
  std::vector<String> fetched;
  std::vector<String> fetchedVisible;
  std::vector<String> fetchedCritical;
  std::vector<String> fetchedInstant;
  parseUriArray("focus", fetchedFocus);
  parseUriArray("uris", fetched);
  parseUriArray("visible", fetchedVisible);
  parseUriArray("critical", fetchedCritical);
  parseUriArray("instant", fetchedInstant);
  criticalPriorityUris = fetchedCritical;
  instantPriorityUris = fetchedInstant;
  if (!sameUriList(visiblePriorityUris, fetchedVisible)) {
    visiblePriorityUris = fetchedVisible; nextVisibleIndex = 0;
  }

  // Focus URIs are already polled in the first slots. Remove them from the
  // background rotation so the remaining dashboard values can advance.
  std::vector<String> fetchedBackground;
  for (const String &uri : fetched) {
    if (!containsUri(fetchedFocus, uri) && !containsUri(fetchedBackground, uri)) {
      fetchedBackground.push_back(uri);
    }
  }
  bool focusChanged = !sameUriList(focusPriorityUris, fetchedFocus);
  bool backgroundChanged = !sameUriList(priorityUris, fetchedBackground);
  if (focusChanged) {
    focusPriorityUris = fetchedFocus;
    nextFocusIndex = 0;
  } else if (!focusPriorityUris.empty() && nextFocusIndex >= focusPriorityUris.size()) {
    nextFocusIndex = 0;
  }
  if (backgroundChanged) {
    priorityUris = fetchedBackground;
    nextPriorityIndex = 0;
  } else if (!priorityUris.empty() && nextPriorityIndex >= priorityUris.size()) {
    nextPriorityIndex = 0;
  }
  persistentReadingUris = fetchedFocus;
  for (const String &uri : fetchedBackground) if (!containsUri(persistentReadingUris, uri)) persistentReadingUris.push_back(uri);
  if (HEATSYNC_TRACE_REQUESTS) Serial.printf("Prioritaeten geladen: Fokus %u, Hintergrund %u\n", (unsigned) focusPriorityUris.size(), (unsigned) priorityUris.size());
  lastPriorityFetch = millis();
  return true;
}

size_t findPointIndexByUri(File &pointsFile, const String &uri) {
  const uint32_t hash = esp_rom_crc32_le(0, (const uint8_t *) uri.c_str(), uri.length());
  for (size_t i = 0; i < etaPointCount; i++) {
    // Keep only a small hash index in RAM; verify the full URI on a match.
    // Priority reads previously scanned hundreds of full SPIFFS records each.
    if (pointURIHashes.size() == etaPointCount && pointURIHashes[i] != hash) continue;
    pointsFile.seek(i * sizeof(HeatSyncPoint));
    HeatSyncPoint point{};
    if (pointsFile.read((uint8_t *) &point, sizeof(point)) != sizeof(point)) break;
    if (uri == String(point.uri)) return i;
  }
  return (size_t) -1;
}

void probeInternet() { String body; int code = cloudRequest("/health.php", "", body); Serial.printf("Cloud HTTP %d\n", code); }

bool pollHeatSyncAndUpload() {
  if (WiFi.status() != WL_CONNECTED) {
    setLastError("Kein WLAN beim Poll");
    return false;
  }
  if (etaBaseUrl.length() == 0) {
    setLastError("Keine Heizungsadresse gespeichert");
    return false;
  }
  if (etaPointCount == 0 || millis() - lastDiscovery > 6UL * 60UL * 60UL * 1000UL) {
    discoverMenu();
    lastDiscovery = millis();
    nextPointIndex = 0;
  }

  runtimePhase.store(1);
  HeatSyncUploadBuffer uploadFile;
  File pointsFile = SPIFFS.open(HEATSYNC_POINTS_FILE, FILE_READ);
  size_t valueCount = 0;
  size_t nextCatalogUploadIndex = catalogUploadIndex;
  if (pointReadAt.size() != etaPointCount) pointReadAt.assign(etaPointCount, 0);
  if (pointsFile && etaPointCount > 0) {
    uploadFile.print("{\"device\":\"ESP32 HeatSync\",\"etaBaseUrl\":\"");
    uploadFile.print(jsonEscape(etaBaseUrl));
    if (dashboardPassword.length() > 0) {
      uploadFile.print("\",\"dashboardPassword\":\"");
      uploadFile.print(jsonEscape(dashboardPassword));
    }
    uploadFile.print("\",\"uptimeSeconds\":");
    uploadFile.print(millis() / 1000);
    uploadFile.print(",\"wifiRssi\":");
    uploadFile.print(WiFi.RSSI());
    if (catalogPending) {
      uploadFile.print(",\"catalog\":[");
      pointsFile.seek(catalogUploadIndex * sizeof(HeatSyncPoint));
      for (size_t i = catalogUploadIndex; i < std::min(catalogUploadIndex + (size_t) 16, etaPointCount); i++) {
        HeatSyncPoint point{};
        if (pointsFile.read((uint8_t *) &point, sizeof(point)) != sizeof(point)) break;
        if (i > catalogUploadIndex) uploadFile.print(",");
        uploadFile.print("{\"uri\":\"");
        uploadFile.print(jsonEscape(String(point.uri)));
        uploadFile.print("\",\"name\":\"");
        uploadFile.print(jsonEscape(String(point.name)));
        uploadFile.print("\",\"group\":\"");
        uploadFile.print(jsonEscape(String(point.group)));
        uploadFile.print("\",\"path\":\"");
        uploadFile.print(jsonEscape(String(point.path)));
        uploadFile.print("\"}");
        nextCatalogUploadIndex = i + 1;
      }
      uploadFile.print("]");
    }
    uploadFile.print(",\"values\":[");
    size_t chosenIndexes[HEATSYNC_VALUES_PER_BATCH];
    size_t chosenCount = 0;
    size_t normalIndex = nextPointIndex;
    const size_t batchSize = std::min(HEATSYNC_VALUES_PER_BATCH, etaPointCount);
    ++readingBatchNumber;
    auto oldestDue = [&](const std::vector<String> &uris, uint32_t minimumAge) -> size_t {
      size_t result = SIZE_MAX;
      uint32_t oldestAge = 0;
      for (const String &uri : uris) {
        const size_t index = findPointIndexByUri(pointsFile, uri);
        if (index >= etaPointCount) continue;
        heatSyncPreferDuePoint(index, pointReadAt[index], millis(), minimumAge, chosenIndexes, chosenCount, result, oldestAge);
      }
      return result;
    };
    for (size_t i = 0; i < batchSize; i++) {
      if (physicalFactoryResetRequested.load()) break;
      size_t pointIndex = SIZE_MAX;
      const bool catalogSlot = i + 1 == batchSize && readingBatchNumber % 3 == 0;
      if (!catalogSlot) {
        if (i == 0) pointIndex = oldestDue(instantPriorityUris, 900);
        if (pointIndex == SIZE_MAX && i < 2) pointIndex = oldestDue(criticalPriorityUris, 2500);
        if (pointIndex == SIZE_MAX) pointIndex = oldestDue(visiblePriorityUris, 2500);
        if (pointIndex == SIZE_MAX) pointIndex = oldestDue(persistentReadingUris, 5000);
      }
      if (i == 0 && pendingControlRefreshURI.length()) {
        size_t commandedIndex = findPointIndexByUri(pointsFile, pendingControlRefreshURI);
        if (commandedIndex != (size_t) -1) pointIndex = commandedIndex;
        pendingControlRefreshURI = "";
      }
      // Do not request the same ETA point twice in one upload batch when a
      // priority point happens to equal the round-robin point.
      bool duplicatePoint = false;
      for (size_t chosen = 0; chosen < chosenCount; chosen++) if (chosenIndexes[chosen] == pointIndex) duplicatePoint = true;
      if (pointIndex == SIZE_MAX || duplicatePoint) pointIndex = heatSyncNextCatalogPoint(normalIndex, etaPointCount, chosenIndexes, chosenCount);
      if (pointIndex == SIZE_MAX) break;
      chosenIndexes[chosenCount++] = pointIndex;
      pointsFile.seek(pointIndex * sizeof(HeatSyncPoint));
      HeatSyncPoint point{};
      if (pointsFile.read((uint8_t *) &point, sizeof(point)) != sizeof(point)) break;
      runtimePhase.store(2);
      String xml = httpGetPlain(etaBaseUrl + "/user/var" + String(point.uri), 2500);
      pointReadAt[pointIndex] = millis();
      runtimePhase.store(3);
      if (HEATSYNC_TRACE_REQUESTS) Serial.printf("ETA GET %s Bytes: %u\n", point.uri, (unsigned) xml.length());
      valueCount = writeUploadValues(xml, uploadFile, valueCount, point.name, point.group, point.path);
      server.handleClient();
      if (i + 1 < HEATSYNC_VALUES_PER_BATCH) delay(HEATSYNC_REQUEST_GAP_MS);
    }
    nextPointIndex = normalIndex;
    uploadFile.print("]}");
    pointsFile.close();
  } else {
    if (pointsFile) pointsFile.close();
  }
  lastValueCount = valueCount;
  if (HEATSYNC_TRACE_REQUESTS) { Serial.print("ETA-Werte gelesen: "); Serial.println(valueCount); }
  if (valueCount == 0) {
    setLastError("ETA liefert keine Werte");
    return false;
  }
  runtimePhase.store(4);
  bool uploaded = uploadToWeb(uploadFile.body);
  runtimePhase.store(0);
  if (uploaded && catalogPending) {
    catalogUploadIndex = nextCatalogUploadIndex;
    catalogPending = catalogUploadIndex < etaPointCount;
  }
  return uploaded;
}

void handleSetupRoot() {
  String html = R"HTML(
<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HeatSync Setup</title><style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#101418;color:#f2f5f7}main{max-width:760px;margin:auto;padding:24px 16px}
section{border:1px solid #2d3944;background:#151b21;margin:14px 0;padding:16px;border-radius:8px}label{display:block;margin:12px 0 6px;color:#aebdca}
select,input,button{box-sizing:border-box;width:100%;font-size:16px;padding:12px;border-radius:8px;border:1px solid #3a4650;background:#0f1419;color:white}
button{background:#f27b35;border:0;font-weight:800;margin-top:12px;color:#111}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.small{color:#9fb0be;font-size:14px}code{color:#ffd6a8}
</style></head><body><main>
<h1>HeatSync Setup</h1><p class="small">Verbinde den ESP32 mit deinem WLAN und waehle danach die lokale Heizungsadresse.</p>
<section><h2>1. WLAN</h2><button onclick="loadWifi()">WLANs suchen</button><label>Netzwerk</label><select id="ssid"></select><label>Passwort</label><input id="pass" type="password" autocomplete="current-password"><button onclick="connectWifi()">Verbinden</button><p id="wifiStatus" class="small"></p></section>
<section><h2>2. ETA Geraet</h2><button onclick="scanDevices()">ETA im Netzwerk suchen</button><label>Gefundene Geraete</label><select id="etaList"></select><label>Oder manuell</label><input id="etaManual" placeholder="http://192.168.1.50:8080"><button onclick="saveHeatSync()">Heizungsadresse speichern</button><p id="etaStatus" class="small"></p></section>
<section><h2>Status</h2><p class="small" id="status">Lade...</p></section>
</main><script>
async function json(url,opts){const r=await fetch(url,opts);return await r.json()}
function opt(text,value){const o=document.createElement('option');o.textContent=text;o.value=value;return o}
async function loadWifi(){wifiStatus.textContent='Suche WLANs...';const d=await json('/api/wifi');ssid.innerHTML='';(d.networks||[]).forEach(n=>ssid.appendChild(opt(`${n.ssid} (${n.rssi} dBm)`,n.ssid)));wifiStatus.textContent=`${(d.networks||[]).length} Netzwerke gefunden`}
async function connectWifi(){wifiStatus.textContent='Verbinde...';const body=new URLSearchParams({ssid:ssid.value,pass:pass.value});const d=await json('/api/connect',{method:'POST',body});wifiStatus.textContent=d.ok?`Verbunden: ${d.ip}`:`Fehler: ${d.error||'keine Verbindung'}`}
function wait(ms){return new Promise(resolve=>setTimeout(resolve,ms))}
async function scanDevices(){etaStatus.textContent='Starte Suche...';etaList.innerHTML='';await json('/api/devices?start=1');let done=false;while(!done){const d=await json('/api/devices');etaList.innerHTML='';(d.devices||[]).forEach(x=>etaList.appendChild(opt(x.label,x.url)));done=!!d.done;etaStatus.textContent=done?`${(d.devices||[]).length} Kandidaten gefunden`:`Suche ${d.scanned||0}/254 - ${(d.devices||[]).length} gefunden`;await wait(180)}}
async function saveHeatSync(){const eta=etaManual.value||etaList.value;etaStatus.textContent='Pruefe und speichere...';const body=new URLSearchParams({eta});const d=await json('/api/save_eta',{method:'POST',body});etaStatus.textContent=d.ok?'Gespeichert. Der ESP32 startet jetzt den Live-Upload.':`Fehler: ${d.error||'ETA nicht erreichbar'}`}
async function loadStatus(){const d=await json('/api/status');status.innerHTML=`WLAN: <code>${d.wifi}</code><br>IP: <code>${d.ip||'-'}</code><br>ETA: <code>${d.eta||'-'}</code><br>Werte: <code>${d.points}</code>`}
loadWifi();loadStatus();setInterval(loadStatus,3000);
</script></body></html>
)HTML";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleWifiScan() {
  int count = WiFi.scanNetworks(false, true);
  String json = "{\"networks\":[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleConnectWifi() {
  wifiSsid = server.arg("ssid");
  wifiPass = server.arg("pass");
  if (wifiSsid.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid fehlt\"}");
    return;
  }
  bool ok = connectWifi(20000);
  if (ok) {
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    server.send(200, "application/json", "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}");
  } else {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"WLAN Verbindung fehlgeschlagen\"}");
  }
}

bool probeHeatSyncIp(IPAddress ip, String &urlOut) {
  WiFiClient client;
  client.setTimeout(180);
  if (!client.connect(ip, 8080, 180)) return false;
  client.print("GET /user/api HTTP/1.1\r\nHost: ");
  client.print(ip.toString());
  client.print("\r\nConnection: close\r\n\r\n");
  uint32_t start = millis();
  String body;
  while (client.connected() && millis() - start < 900) {
    while (client.available()) body += (char) client.read();
    delay(1);
  }
  client.stop();
  body.toLowerCase();
  if (body.indexOf("<api") >= 0 || body.indexOf("eta") >= 0) {
    urlOut = "http://" + ip.toString() + ":8080";
    return true;
  }
  return false;
}

String deviceScanResponseJson(bool done) {
  String json = "{\"done\":";
  json += done ? "true" : "false";
  json += ",\"scanned\":";
  json += String(deviceScanNextHost > 254 ? 254 : deviceScanNextHost - 1);
  json += ",\"devices\":[";
  for (size_t i = 0; i < discoveredDevices.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"label\":\"" + jsonEscape(discoveredDevices[i].label) + "\",\"url\":\"" + jsonEscape(discoveredDevices[i].url) + "\"}";
  }
  json += "]}";
  return json;
}

void handleDeviceScan() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(200, "application/json", "{\"done\":true,\"scanned\":0,\"devices\":[],\"error\":\"WLAN nicht verbunden\"}");
    return;
  }

  if (server.hasArg("start") || !deviceScanRunning) {
    discoveredDevices.clear();
    deviceScanNextHost = 1;
    deviceScanRunning = true;
  }

  IPAddress local = WiFi.localIP();
  IPAddress subnet(local[0], local[1], local[2], 0);
  uint32_t sliceStarted = millis();
  uint8_t checkedThisRequest = 0;

  while (deviceScanRunning && deviceScanNextHost <= 254 && checkedThisRequest < 10 && millis() - sliceStarted < 2600) {
    IPAddress ip(subnet[0], subnet[1], subnet[2], deviceScanNextHost);
    deviceScanNextHost++;
    checkedThisRequest++;
    if (ip == local) continue;

    String url;
    if (probeHeatSyncIp(ip, url)) {
      discoveredDevices.push_back({"ETA Kandidat " + ip.toString(), url});
    }
    delay(1);
  }

  if (deviceScanNextHost > 254) {
    deviceScanRunning = false;
  }

  server.send(200, "application/json", deviceScanResponseJson(!deviceScanRunning));
}

void runDeviceScanSlice() {
  if (!deviceScanRunning || WiFi.status() != WL_CONNECTED) return;

  IPAddress local = WiFi.localIP();
  IPAddress subnet(local[0], local[1], local[2], 0);
  uint32_t sliceStarted = millis();
  uint8_t checkedThisLoop = 0;

  while (deviceScanRunning && deviceScanNextHost <= 254 && checkedThisLoop < 4 && millis() - sliceStarted < 900) {
    IPAddress ip(subnet[0], subnet[1], subnet[2], deviceScanNextHost);
    deviceScanNextHost++;
    checkedThisLoop++;
    if (ip == local) continue;

    String url;
    if (probeHeatSyncIp(ip, url)) {
      discoveredDevices.push_back({"ETA Kandidat " + ip.toString(), url});
      notifyBleStatus("eta candidate found");
    }
    delay(1);
  }

  if (deviceScanNextHost > 254) {
    deviceScanRunning = false;
    notifyBleStatus("eta scan done");
  }
}

void handleSaveHeatSync() {
  String eta = normalizeBaseUrl(server.arg("eta"));
  if (eta.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"ETA Adresse fehlt\"}");
    return;
  }
  if (!etaLooksAlive(eta)) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"ETA API auf /user/api nicht erreichbar\"}");
    return;
  }
  etaBaseUrl = eta;
  configured = deviceToken.length() == 64 && !serviceURL.isEmpty();
  saveConfig();
  discoverMenu();
  pollHeatSyncAndUpload();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  String wifi = WiFi.status() == WL_CONNECTED ? "verbunden" : "nicht verbunden";
  String json = "{\"wifi\":\"" + wifi + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"eta\":\"" + jsonEscape(etaBaseUrl) + "\",\"points\":" + String(etaPointCount) + ",\"values\":" + String(lastValueCount) + ",\"uploadCode\":" + String(lastUploadCode) + ",\"lastError\":\"" + jsonEscape(lastError) + "\"}";
  json.remove(json.length() - 1);
  json += ",\"uptimeSeconds\":" + String(millis() / 1000);
  json += ",\"lastUploadSeconds\":" + (lastUploadAt == 0 ? String("null") : String((millis() - lastUploadAt) / 1000));
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"lastRecoveryPhase\":" + String(lastRecoveryPhase);
  json += ",\"loopRecoveryEnabled\":" + String(runtimeWatchdogArmed.load() ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

String bleStatusJson(String message = "") {
  String wifi = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  String json = "{";
  json += "\"wifi\":\"" + wifi + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"eta\":\"" + jsonEscape(etaBaseUrl) + "\",";
  json += "\"configured\":";
  json += configured ? "true" : "false";
  json += ",\"points\":" + String(etaPointCount);
  json += ",\"deviceID\":\"" + hardwareID() + "\",\"cloudProvisioned\":"; json += deviceToken.length() == 64 ? "true" : "false";
  json += ",\"networks\":" + wifiNetworksJson;
  json += ",\"dashboardPasswordSet\":";
  json += dashboardPassword.length() > 0 ? "true" : "false";
  json += ",\"scanRunning\":";
  json += deviceScanRunning ? "true" : "false";
  json += ",\"scanned\":";
  json += String(deviceScanNextHost > 254 ? 254 : deviceScanNextHost - 1);
  json += ",\"devices\":[";
  for (size_t i = 0; i < discoveredDevices.size(); i++) {
    if (i > 0) json += ",";
    json += "{\"label\":\"" + jsonEscape(discoveredDevices[i].label) + "\",\"url\":\"" + jsonEscape(discoveredDevices[i].url) + "\"}";
  }
  json += "]";
  json += ",\"values\":" + String(lastValueCount);
  json += ",\"uploadCode\":" + String(lastUploadCode);
  json += ",\"lastUploadSeconds\":";
  json += lastUploadAt == 0 ? "null" : String((millis() - lastUploadAt) / 1000);
  json += ",\"lastError\":\"" + jsonEscape(lastError) + "\"";
  json += ",\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  return json;
}

void notifyBleStatus(String message) {
  if (!bleActive || !bleStatusCharacteristic || !bleAuthenticated || !bleServerInstance) return;
  String json = bleStatusJson(message);
  if (json.length() > 12288) return;
  // Budget for the ATT header, the PART framing and base64 at the negotiated MTU.
  size_t mtu = bleServerInstance->getPeerMTU(bleServerInstance->getConnId());
  if (mtu < 23) mtu = 23;
  size_t chunkSize = ((mtu - 3 - 16) / 4) * 3;
  if (chunkSize > 96) chunkSize = 96;
  size_t total = (json.length() + chunkSize - 1) / chunkSize;
  for (size_t index = 0; index < total; ++index) {
    if (!bleActive || !bleAuthenticated || (int32_t)(millis() - setupDeadline) >= 0) return;
    String part = json.substring(index * chunkSize, (index + 1) * chunkSize); unsigned char encoded[132]; size_t count = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &count, (const unsigned char *) part.c_str(), part.length()) != 0) return;
    String frame = "PART|" + String(index) + "|" + String(total) + "|"; frame.concat((const char *) encoded, count);
    bleStatusCharacteristic->setValue(frame.c_str()); bleStatusCharacteristic->notify(); delay(25);
  }
}

void requestRestart(uint32_t delayMs = 1200) {
  restartRequested = true;
  restartAt = millis() + delayMs;
}

void handleBleCommand(String command) {
  if (!bleActive || !bleAuthenticated || !setupDeadline || (int32_t)(millis() - setupDeadline) >= 0) return;
  command.trim();
  if (command.startsWith("PROVISION|")) {
    bool ok = signedProvisioning(command.substring(10)); notifyBleStatus(ok ? "cloud configured" : "invalid signed provisioning"); return;
  }
  if (command.startsWith("BEGIN|")) { provisioningBuffer = ""; provisioningBuffer.reserve(4096); return; }
  if (command.startsWith("CHUNK|")) { if (provisioningBuffer.length() + command.length() - 6 <= 4096) provisioningBuffer += command.substring(6); else provisioningBuffer = ""; return; }
  if (command == "COMMIT") { bool ok = signedProvisioning(provisioningBuffer); provisioningBuffer = ""; notifyBleStatus(ok ? "cloud configured" : "invalid signed provisioning"); return; }
  if (command == "WIFISCAN") {
    notifyBleStatus("scanning wifi");
    int count = WiFi.scanNetworks(false, false);
    wifiNetworksJson = "["; std::vector<String> names;
    for (int index = 0; index < count && names.size() < 24; ++index) {
      String ssid = WiFi.SSID(index); bool duplicate = false;
      for (const String &name : names) { if (name == ssid) duplicate = true; }
      if (ssid.isEmpty() || duplicate) continue;
      if (!names.empty()) wifiNetworksJson += ",";
      names.push_back(ssid);
      wifiNetworksJson += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(index)) + "}";
    }
    wifiNetworksJson += "]"; WiFi.scanDelete();
    notifyBleStatus(count < 0 ? "wifi scan failed" : "wifi scan done"); return;
  }
  if (command == "PING") {
    notifyBleStatus("pong");
    return;
  }

  if (command.startsWith("WIFI|")) {
    int separator = command.indexOf('|', 5);
    if (separator < 0) {
      notifyBleStatus("wifi command invalid");
      return;
    }
    wifiSsid = urlDecode(command.substring(5, separator));
    wifiPass = urlDecode(command.substring(separator + 1));
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    notifyBleStatus("connecting wifi");
    bool ok = connectWifi(20000);
    notifyBleStatus(ok ? "wifi connected" : "wifi failed");
    return;
  }

  if (command.startsWith("HEATER|")) {
    if (deviceToken.length() != 64) { notifyBleStatus("cloud configuration required"); return; }
    String eta = normalizeBaseUrl(urlDecode(command.substring(7)));
    notifyBleStatus("checking eta");
    if (!etaLooksAlive(eta)) {
      notifyBleStatus("eta not reachable");
      return;
    }
    etaBaseUrl = eta;
    configured = true;
    saveConfig();
    notifyBleStatus("eta saved, restarting");
    requestRestart();
    return;
  }

  if (command.startsWith("DASHPASS|")) {
    String pass = urlDecode(command.substring(9));
    pass.trim();
    if (pass.length() < 4) {
      notifyBleStatus("dashboard password too short");
      return;
    }
    dashboardPassword = pass;
    saveConfig();
    notifyBleStatus("dashboard password saved");
    lastPoll = millis() - POLL_INTERVAL_MS;
    if (!setupMode) bleReleaseAt = millis() + 15000;
    return;
  }

  if (command == "SCAN") {
    if (WiFi.status() != WL_CONNECTED) {
      notifyBleStatus("wifi required for scan");
      return;
    }
    discoveredDevices.clear();
    deviceScanNextHost = 1;
    deviceScanRunning = true;
    notifyBleStatus("eta scan started");
    return;
  }

  if (command == "RESET") {
    notifyBleStatus("reset requires account owner");
    return;
  }

  notifyBleStatus("unknown command");
}

void processPendingBleCommand() {
  String *command = nullptr;
  if (!bleCommands || xQueueReceive(bleCommands, &command, 0) != pdTRUE) return;
  handleBleCommand(*command); delete command;
  hasPendingBleCommand = uxQueueMessagesWaiting(bleCommands) > 0;
}

class HeatSyncBleWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    if (!bleAuthenticated || !setupDeadline || (int32_t)(millis() - setupDeadline) >= 0) return;
    lastBleInteraction = millis();
    auto value = characteristic->getValue();
    if (value.length() > 512 || !bleCommands) return;
    String *command = new String(value.c_str());
    if (xQueueSend(bleCommands, &command, 0) != pdTRUE) { delete command; return; }
    hasPendingBleCommand = true;
  }
};

class HeatSyncBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleAuthenticated = false;
    lastBleInteraction = millis();
    Serial.println("HeatSync BLE: Verbindung, sichere Kopplung wird aufgebaut");
  }

  void onDisconnect(BLEServer *server) override {
    bleAuthenticated = false;
    bleStatusAfterAuthentication = false;
    if (bleActive && setupDeadline && (int32_t)(millis() - setupDeadline) < 0) BLEDevice::startAdvertising();
  }
};

class HeatSyncSecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return setupDeadline && (int32_t)(millis() - setupDeadline) < 0; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
    bleAuthenticated = result.success;
    bleStatusAfterAuthentication = result.success;
    Serial.printf("HeatSync BLE: Kopplung %s, Grund %u\n", result.success ? "bestaetigt" : "fehlgeschlagen", result.fail_reason);
  }
};
void startBleProvisioning() {
  setupDeadline = millis() + 10UL * 60UL * 1000UL;
  bleCommands = xQueueCreate(24, sizeof(String *));
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(185);
  BLEDevice::setSecurityCallbacks(new HeatSyncSecurityCallbacks());
  // The three-argument API also enables the Arduino 3.3.x security lifecycle.
  // The legacy bitmask overload does not set its m_securityEnabled flag.
  BLESecurity *security = new BLESecurity(); security->setAuthenticationMode(true, false, true);
  security->setCapability(ESP_IO_CAP_NONE);
  bleActive = true;
  lastBleInteraction = millis();
  BLEServer *bleServer = BLEDevice::createServer();
  bleServerInstance = bleServer;
  bleServer->setCallbacks(new HeatSyncBleServerCallbacks());
  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  BLECharacteristic *writeCharacteristic = service->createCharacteristic(
    BLE_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  writeCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  writeCharacteristic->setCallbacks(new HeatSyncBleWriteCallbacks());

  bleStatusCharacteristic = service->createCharacteristic(
    BLE_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleStatusCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  bleStatusCharacteristic->addDescriptor(new BLE2902());
  bleStatusCharacteristic->setValue(bleStatusJson("ready").c_str());

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  // A factory reset erases bonds. A fresh BLE identity avoids reusing stale
  // phone pairing keys; the server hardware ID remains the eFuse identity.
  esp_bd_addr_t address;
  if (prefs.getBytesLength("bleAddress") != sizeof(address)) {
    if (esp_ble_gap_addr_create_static(address) != ESP_OK ||
        prefs.putBytes("bleAddress", address, sizeof(address)) != sizeof(address)) {
      setLastError("Bluetooth-Identitaet konnte nicht gespeichert werden");
      releaseBleAfterSetup(); return;
    }
  } else prefs.getBytes("bleAddress", address, sizeof(address));
  if (!advertising->setDeviceAddress(address, BLE_ADDR_TYPE_RANDOM)) {
    setLastError("Bluetooth-Identitaet konnte nicht gestartet werden");
    releaseBleAfterSetup(); return;
  }
  // Keep the 128-bit service and full name in separate 31-byte packets.
  BLEAdvertisementData advertisement;
  advertisement.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advertisement.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
  BLEAdvertisementData response;
  response.setName(BLE_DEVICE_NAME);
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(response);
  advertising->setScanResponse(true);
  if (!advertising->start()) {
    setLastError("Bluetooth-Einrichtung konnte nicht gestartet werden");
    releaseBleAfterSetup(); return;
  }
  statusLEDMode = StatusLEDMode::setup;
  Serial.printf("HeatSync BLE: %s, Einrichtung fuer 10 Minuten offen\n", BLE_DEVICE_NAME);
}

void releaseBleAfterSetup() {
  // BLE is only needed during provisioning. Releasing it leaves enough heap
  // for the ESP32 TLS client used by the HTTPS upload.
  bleActive = false; bleAuthenticated = false;
  bleStatusAfterAuthentication = false;
  statusLEDMode = StatusLEDMode::off;
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);
  bleStatusCharacteristic = nullptr;
  bleServerInstance = nullptr;
  bleActive = false;
  bleReleaseAt = 0;
  delay(100);
  Serial.printf("BLE freigegeben, Heap: %u\n", (unsigned) ESP.getFreeHeap());
}

void startSetupPortal() { setupMode = true; WiFi.mode(WIFI_STA); Serial.println("HeatSync: Einrichtung nur per verschluesseltem Bluetooth"); }

void startStatusServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain; charset=utf-8", factoryResetID.isEmpty()
      ? "HeatSync laeuft. Werksreset: BOOT 5 Sekunden halten und loslassen. Der ESP32 wird auch vom Serverkonto geloest; Heizungsdaten bleiben gespeichert."
      : "HeatSync: Werksreset wartet auf Internet/Serverbestaetigung. Keine Heizungsabfragen oder Schaltbefehle. Bisheriges WLAN eingeschaltet lassen.");
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.begin();
}

#include "HeatSyncControls.h"

void monitorFactoryResetButton(void *) {
  FactoryResetButton button;
  for (;;) {
    if (heatSyncLoopStalled(millis(), runtimeProgressAt.load(), runtimeWatchdogArmed.load())) {
      // Restart only; credentials, ownership and pending reset receipts survive.
      lastRecoveryPhase = runtimePhase.load();
      recoveryMagic = 0x4853594e;
      ESP.restart();
    }
    const bool wasReady = button.ready();
    if (button.update(millis(), digitalRead(SETUP_BUTTON_PIN) == LOW)) {
      physicalFactoryResetRequested.store(true);
    }
    if (!wasReady && button.ready()) Serial.println("HeatSync: BOOT loslassen fuer lokalen Werksreset");
    const bool light = statusLEDOn(statusLEDMode.load(), millis(), button.ready());
    digitalWrite(STATUS_LED_PIN, light == STATUS_LED_ACTIVE_HIGH ? HIGH : LOW);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void clearFactorySettingsAndRestart() {
  runtimeWatchdogArmed.store(false);
  // Only the main task erases storage, between completed network/file operations.
  Serial.println("HeatSync: Werkseinstellungen werden geloescht");
  statusLEDMode = StatusLEDMode::resetPending;
  server.stop();
  if (bleActive) releaseBleAfterSetup();
  WiFi.disconnect(true, true);
  const bool preferencesCleared = prefs.clear(); prefs.end();
  const bool filesCleared = SPIFFS.format(); SPIFFS.end();
  nvs_flash_deinit();
  const esp_err_t result = nvs_flash_erase();
  if (!preferencesCleared || !filesCleared || result != ESP_OK) {
    Serial.printf("HeatSync: Werksreset unvollstaendig (Prefs %d, Dateien %d, NVS %d). Bitte erneut pruefen.\n", preferencesCleared, filesCleared, result);
  } else Serial.println("HeatSync: Werkseinstellungen hergestellt. Neustart zur Einrichtung.");
  Serial.flush(); delay(100); ESP.restart();
}

void performPhysicalFactoryResetIfRequested() {
  if (physicalFactoryResetRequested.exchange(false) && factoryResetID.isEmpty()) {
    uint8_t random[16]; esp_fill_random(random, sizeof(random));
    char id[33]; for (size_t i = 0; i < sizeof(random); ++i) snprintf(id + 2 * i, 3, "%02x", random[i]);
    factoryResetID = id;
    if (prefs.putString("factoryReset", factoryResetID) != factoryResetID.length()) {
      factoryResetID = ""; Serial.println("HeatSync: Resetauftrag konnte nicht gespeichert werden"); return;
    }
    Serial.println("HeatSync: Werksreset angefordert. Serverfreigabe wird bestaetigt.");
    if (bleActive) releaseBleAfterSetup();
  }
  if (factoryResetID.isEmpty()) return;
  statusLEDMode = StatusLEDMode::resetPending;
  if (serviceURL.isEmpty() || deviceToken.isEmpty()) { clearFactorySettingsAndRestart(); return; }
  if (WiFi.status() != WL_CONNECTED || (lastFactoryResetSync && millis() - lastFactoryResetSync < 5000)) return;
  lastFactoryResetSync = millis();
  JsonDocument request; request["action"] = "factoryReset"; request["resetID"] = factoryResetID;
  String body, response; serializeJson(request, body);
  const int code = cloudRequest("/bridge_state.php", body, response);
  JsonDocument confirmation;
  if (code == 200 && !deserializeJson(confirmation, response) && confirmation["released"] == true
      && confirmation["resetID"].as<String>() == factoryResetID) { clearFactorySettingsAndRestart(); return; }
  Serial.printf("HeatSync: Werksreset wartet auf Serverbestaetigung, HTTP %d\n", code);
}

void pollOwnerReset() {
  if (millis() - lastOwnerResetCheck < 10000 || WiFi.status() != WL_CONNECTED || deviceToken.length() != 64) return;
  lastOwnerResetCheck = millis();
  String response;
  if (ownerResetID.isEmpty()) {
    if (cloudRequest("/bridge_state.php", "", response) != 200) return;
    JsonDocument command; if (deserializeJson(command, response)) return;
    String id = command["resetID"].as<String>(); if (id.length() != 32) return;
    // Persist only an owner-authorized HTTPS command before acknowledging it.
    if (prefs.putString("ownerReset", id) != id.length()) return;
    ownerResetID = id;
  }
  JsonDocument ack; ack["resetID"] = ownerResetID; String body; serializeJson(ack, body);
  int code = cloudRequest("/bridge_state.php", body, response);
  // A lost successful response leaves the old token revoked on retry.
  if (code != 200 && code != 401) return;
  prefs.clear(); SPIFFS.format(); requestRestart(200);
  configured = false; deviceToken = ""; serviceURL = "";
}

void setup() {
  if (recoveryMagic != 0x4853594e) lastRecoveryPhase = 0;
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_HIGH ? LOW : HIGH);
  delay(500);
  if (!SPIFFS.begin(true)) {
    setLastError("ESP32-Speicher konnte nicht gestartet werden");
  }
  SPIFFS.remove(HEATSYNC_UPLOAD_FILE);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  loadConfig();
  factoryResetID = prefs.getString("factoryReset", "");
  if (!factoryResetID.isEmpty()) statusLEDMode = StatusLEDMode::resetPending;
  if (xTaskCreate(monitorFactoryResetButton, "factory-reset", 3072, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("HeatSync: BOOT-Ueberwachung konnte nicht gestartet werden. Reset ueber Besitzerkonto verwenden.");
  }
  ownerResetID = prefs.getString("ownerReset", "");
  prefs.remove("setupNext");
  if (!factoryResetID.isEmpty()) {
    Serial.println("HeatSync: Ausstehender Werksreset wird fortgesetzt, kein Heizungsbetrieb");
    connectWifi(20000); startStatusServer(); performPhysicalFactoryResetIfRequested(); return;
  }
  if (!configured) {
    startBleProvisioning();
    startSetupPortal();
    if (wifiSsid.length() > 0) connectWifi(8000);
    return;
  }

  setupMode = false;
  Serial.println("HeatSync: Konto eingerichtet, Bluetooth bleibt deaktiviert");
  connectWifi(20000);
  performPhysicalFactoryResetIfRequested();
  if (!factoryResetID.isEmpty()) return;
  startStatusServer();
  if (!ownerResetID.isEmpty()) return;
  runtimeProgressAt.store(millis());
  runtimeWatchdogArmed.store(true);
  bleReleaseAt = millis() + 20000;
  probeInternet();
  fetchPrioritiesFromWeb();
  performPhysicalFactoryResetIfRequested();
  if (!factoryResetID.isEmpty()) return;
  etaBaseUrl = normalizeBaseUrl(etaBaseUrl);
  discoverMenu();
  performPhysicalFactoryResetIfRequested();
  if (!factoryResetID.isEmpty()) return;
  pollHeatSyncAndUpload();
  lastPoll = millis();
}

void loop() {
  runtimeProgressAt.store(millis());
  performPhysicalFactoryResetIfRequested();
  if (!factoryResetID.isEmpty()) {
    server.handleClient();
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = millis(); connectWifi(10000);
    }
    return;
  }
  runtimePhase.store(5);
  pollOwnerReset();
  runtimePhase.store(0);
  if (bleActive && setupDeadline && (int32_t)(millis() - setupDeadline) >= 0) releaseBleAfterSetup();
  if (bleStatusAfterAuthentication.exchange(false)) notifyBleStatus("connected");
  server.handleClient();
  processPendingBleCommand();

  if (restartRequested) { if (millis() >= restartAt) ESP.restart(); return; }
  if (!ownerResetID.isEmpty()) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = millis(); connectWifi(10000);
    }
    return;
  }

  if (millis() - lastBleStatus > 5000) {
    lastBleStatus = millis();
    notifyBleStatus();
  }

  runDeviceScanSlice();

  if (!setupMode && bleActive && bleReleaseAt > 0 && millis() >= bleReleaseAt &&
      (!bleServerInstance || bleServerInstance->getConnectedCount() == 0 ||
       (!deviceScanRunning && !hasPendingBleCommand && millis() - lastBleInteraction >= 90000))) {
    releaseBleAfterSetup();
  }

  if (setupMode) {
    dnsServer.processNextRequest();
    return;
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > WIFI_RETRY_MS) {
    lastWifiRetry = millis();
    connectWifi(10000);
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastPriorityFetch >= PRIORITY_REFRESH_MS) {
    lastPriorityFetch = millis();
    runtimePhase.store(6);
    fetchPrioritiesFromWeb();
    runtimePhase.store(0);
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollHeatSyncAndUpload();
  }
  runtimePhase.store(7);
  if (!physicalFactoryResetRequested.load()) processControlCommands();
  runtimePhase.store(0);
}
