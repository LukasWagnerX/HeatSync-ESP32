#pragma once
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyXML.h>
#include <time.h>
#include "HeatSyncCA.h"

struct EtaControlInfo {
  bool error = false, complete = false, writable = false;
  String uri, name, type, onRaw, offRaw, onText, offText;
  String successURI, observedURI, observedRaw, observedText;
  int valueCount = 0, onCount = 0, offCount = 0;
};
static EtaControlInfo controlXmlInfo;
static String controlXmlPath, controlValueLabel;
static uint32_t lastControlPoll = 0;
static bool controlClockStarted = false;
static String pendingControlAck;

static bool controlDigits(const String &value) {
  if (value.length() == 0 || value.length() > 10) return false;
  for (size_t i = 0; i < value.length(); ++i) if (value[i] < '0' || value[i] > '9') return false;
  return true;
}

static bool controlURIValid(const String &uri) {
  if (!uri.startsWith("/") || uri.length() >= 40 || uri.endsWith("/")) return false;
  int segments = 0;
  String part;
  for (size_t i = 0; i <= uri.length(); ++i) {
    if (i == uri.length() || uri[i] == '/') {
      if (i > 0) { if (!controlDigits(part)) return false; ++segments; }
      part = "";
    } else part += uri[i];
  }
  return segments == 5;
}

static void controlXMLCallback(uint8_t flags, char *tag, uint16_t, char *data, uint16_t) {
  String key = tag ? String(tag) : "";
  String value = data ? xmlUnescape(String(data)) : "";
  value.trim();
  if (flags & STATUS_ERROR) { controlXmlInfo.error = true; return; }
  if (flags & STATUS_START_TAG) {
    controlXmlPath = key;
    if (key == "/eta") controlXmlInfo.error = false;
    if (key.endsWith("/validValues/value")) controlValueLabel = "";
  }
  if (flags & STATUS_ATTR_TEXT) {
    if (controlXmlPath == "/eta/varInfo/variable") {
      if (key == "isWritable") controlXmlInfo.writable = value == "1";
      if (key == "uri") controlXmlInfo.uri = normalizeUri(value);
      if (key == "name") controlXmlInfo.name = value;
    } else if (controlXmlPath.endsWith("/validValues/value") && key == "strValue") controlValueLabel = value;
    else if (controlXmlPath == "/eta/success" && key == "uri") controlXmlInfo.successURI = value;
    else if (controlXmlPath == "/eta/value" && key == "strValue") controlXmlInfo.observedText = value;
    else if (controlXmlPath == "/eta/value" && key == "uri") controlXmlInfo.observedURI = value;
  }
  if (flags & STATUS_TAG_TEXT) {
    if (key == "/eta/varInfo/variable/type") controlXmlInfo.type = value;
    if (key == "/eta/value") controlXmlInfo.observedRaw = value;
    if (key.endsWith("/validValues/value")) {
      ++controlXmlInfo.valueCount;
      String label = controlValueLabel; label.toLowerCase();
      if (controlDigits(value) && (label == "ein" || label == "on" || label == "ja" || label == "true")) {
        controlXmlInfo.onRaw = value; controlXmlInfo.onText = controlValueLabel; ++controlXmlInfo.onCount;
      }
      if (controlDigits(value) && (label == "aus" || label == "off" || label == "nein" || label == "false")) {
        controlXmlInfo.offRaw = value; controlXmlInfo.offText = controlValueLabel; ++controlXmlInfo.offCount;
      }
    }
  }
  if (flags & STATUS_END_TAG) {
    if (key == "/eta") controlXmlInfo.complete = true;
    int slash = key.lastIndexOf('/');
    controlXmlPath = slash >= 0 ? key.substring(0, slash) : "";
  }
}

static EtaControlInfo controlParseXML(const String &xml) {
  controlXmlInfo = EtaControlInfo(); controlXmlPath = ""; controlValueLabel = "";
  uint8_t buffer[512]; TinyXML parser;
  parser.init(buffer, sizeof(buffer), controlXMLCallback);
  for (size_t i = 0; i < xml.length(); ++i) parser.processChar((uint8_t) xml[i]);
  return controlXmlInfo;
}

static bool controlIsBinary(const EtaControlInfo &info, const String &uri) {
  String name = info.name; name.toLowerCase();
  bool ordinarySwitch = name.indexOf("ein/aus") >= 0 || name.indexOf("on/off") >= 0;
  return info.complete && !info.error && info.uri == uri && info.writable && info.type == "TEXT" && ordinarySwitch &&
    info.valueCount == 2 && info.onCount == 1 && info.offCount == 1 && info.onRaw != info.offRaw;
}

static int controlServerRequest(const String &post, String &response) { return cloudRequest("/controls.php", post, response); }

static void processControlCommands() {
  if (physicalFactoryResetRequested.load() || !factoryResetID.isEmpty()) return;
  if (bleActive || WiFi.status() != WL_CONNECTED || etaBaseUrl.length() == 0 || millis() - lastControlPoll < 3000) return;
  lastControlPoll = millis();
  if (!controlClockStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); controlClockStarted = true; }
  if (time(nullptr) < 1700000000) {
    static uint32_t lastClockNotice = 0;
    if (millis() - lastClockNotice > 30000) { lastClockNotice = millis(); Serial.println("Control: waiting for network clock (NTP)"); }
    return; // No insecure TLS fallback.
  }
  String response;
  if (pendingControlAck.length()) {
    int code = controlServerRequest(pendingControlAck, response);
    if ((code >= 200 && code < 300) || code == 409) pendingControlAck = "";
    return;
  }
  if (controlServerRequest("", response) != 200) return;
  if (physicalFactoryResetRequested.load()) return;
  JsonDocument input;
  if (deserializeJson(input, response) || !input["command"].is<JsonObject>()) return;
  JsonObject command = input["command"].as<JsonObject>();
  String id = command["id"].as<String>(), uri = command["uri"].as<String>(), kind = command["kind"].as<String>();
  JsonDocument reply; reply["id"] = id; reply["status"] = "failed";
  if (!controlURIValid(uri) || id.length() != 36 || (kind != "inspect" && kind != "set")) reply["error"] = "invalid_command";
  else if (command["expiresAt"].as<long long>() < (long long) time(nullptr)) reply["error"] = "command_expired";
  else {
    delay(HEATSYNC_REQUEST_GAP_MS);
    if (physicalFactoryResetRequested.load()) return;
    EtaControlInfo info = controlParseXML(httpGetPlain(etaBaseUrl + "/user/varinfo" + uri, 2500));
    bool binary = controlIsBinary(info, uri);
    if (kind == "inspect") {
      reply["status"] = info.complete && !info.error && info.uri == uri ? "succeeded" : "failed";
      JsonObject capability = reply["capability"].to<JsonObject>();
      capability["uri"] = uri; capability["writable"] = info.writable; capability["binary"] = binary;
      capability["name"] = info.name; capability["onRaw"] = info.onRaw; capability["offRaw"] = info.offRaw;
      capability["onText"] = info.onText; capability["offText"] = info.offText;
      if (info.complete && !info.error && info.uri == uri) {
        delay(HEATSYNC_REQUEST_GAP_MS);
        EtaControlInfo observed = controlParseXML(httpGetPlain(etaBaseUrl + "/user/var" + uri, 2500));
        if (observed.complete && !observed.error && observed.observedURI == "/user/var" + uri && observed.observedRaw.length()) {
          JsonObject reading = reply["reading"].to<JsonObject>();
          reading["raw"] = observed.observedRaw; reading["text"] = observed.observedText;
          reading["observedAt"] = (long long) time(nullptr);
        } else { reply["status"] = "failed"; reply["error"] = "state_read_failed"; }
        pendingControlRefreshURI = uri; lastPoll = 0;
      }
    } else {
      String raw = command["rawValue"].as<String>();
      if (!binary || (raw != info.onRaw && raw != info.offRaw)) reply["error"] = "not_a_verified_switch";
      else if (command["expiresAt"].as<long long>() < (long long) time(nullptr)) reply["error"] = "command_expired";
      else if (prefs.getString("ctrlLast", "") == id) reply["error"] = "command_already_executed";
      else {
        prefs.putString("ctrlLast", id); // Persist BEFORE POST; never replay on reboot.
        delay(HEATSYNC_REQUEST_GAP_MS);
        if (physicalFactoryResetRequested.load()) return;
        WiFiClient client; HTTPClient http; http.setTimeout(2500); http.setConnectTimeout(2500);
        if (!http.begin(client, etaBaseUrl + "/user/var" + uri)) reply["error"] = "eta_connect_failed";
        else {
          http.addHeader("Content-Type", "application/x-www-form-urlencoded");
          if (physicalFactoryResetRequested.load()) { http.end(); return; }
          int code = http.POST("value=" + raw);
          String body = code > 0 ? http.getString() : ""; http.end();
          EtaControlInfo result = controlParseXML(body);
          if (code == 200 && result.complete && !result.error && result.successURI == "/user/var" + uri) {
            delay(HEATSYNC_REQUEST_GAP_MS);
            EtaControlInfo observed = controlParseXML(httpGetPlain(etaBaseUrl + "/user/var" + uri, 2500));
            bool validReading = observed.complete && !observed.error && observed.observedURI == "/user/var" + uri && observed.observedRaw.length();
            reply["status"] = validReading && observed.observedRaw == raw ? "succeeded" : "uncertain";
            if (validReading) {
              JsonObject reading = reply["reading"].to<JsonObject>();
              reading["raw"] = observed.observedRaw; reading["text"] = observed.observedText;
              reading["observedAt"] = (long long) time(nullptr);
            }
            if (reply["status"] == "uncertain") reply["error"] = "readback_not_confirmed";
            pendingControlRefreshURI = uri;
            lastPoll = 0;
          } else {
            reply["status"] = code < 0 ? "uncertain" : "failed";
            reply["error"] = "eta_write_not_confirmed";
          }
        }
      }
    }
  }
  serializeJson(reply, pendingControlAck);
  Serial.printf("Control %s %s -> %s\n", kind.c_str(), uri.c_str(), reply["status"].as<const char *>());
}
