#pragma once
#include <HTTPClient.h>
#include <time.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include "HeatSyncCA.h"
#include "ProvisioningPublicKey.h"

String serviceURL, deviceToken, heaterID, provisioningBuffer;
static uint32_t cloudBackoffUntil = 0;
static String hardwareID() {
  char value[13]; snprintf(value, sizeof(value), "%012llx", (unsigned long long) ESP.getEfuseMac()); return String(value);
}
static bool clockReady() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 50 && time(nullptr) < 1700000000; ++i) delay(100);
  return time(nullptr) > 1700000000;
}
static bool signedProvisioning(const String &packet) {
  JsonDocument envelope;
  if (deserializeJson(envelope, packet)) return false;
  String config = envelope["configuration"].as<String>(), signature = envelope["signature"].as<String>();
  if (config.length() > 2048 || signature.length() > 200 || !clockReady()) return false;
  uint8_t digest[32], decoded[128]; size_t length = 0;
  mbedtls_sha256((const unsigned char *) config.c_str(), config.length(), digest, 0);
  if (mbedtls_base64_decode(decoded, sizeof(decoded), &length, (const unsigned char *) signature.c_str(), signature.length())) return false;
  mbedtls_pk_context key; mbedtls_pk_init(&key);
  int result = mbedtls_pk_parse_public_key(&key, (const unsigned char *) HEATSYNC_PROVISIONING_PUBLIC_KEY, strlen(HEATSYNC_PROVISIONING_PUBLIC_KEY) + 1);
  if (result == 0) result = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), decoded, length);
  mbedtls_pk_free(&key);
  if (result != 0) return false;
  JsonDocument data; if (deserializeJson(data, config)) return false;
  String url = data["serviceURL"].as<String>(), token = data["deviceToken"].as<String>();
  if (data["deviceID"].as<String>() != hardwareID() || data["expiresAt"].as<long>() <= time(nullptr)
      || !url.startsWith("https://") || url.indexOf('@') >= 0 || url.indexOf('?') >= 0 || url.length() > 200 || token.length() != 64) return false;
  serviceURL = url; deviceToken = token; heaterID = data["heaterID"].as<String>();
  prefs.putString("service", serviceURL); prefs.putString("token", deviceToken); prefs.putString("heater", heaterID);
  return true;
}
static int cloudRequest(const String &endpoint, const String &post, String &body, File *file = nullptr) {
  if (serviceURL.length() == 0 || deviceToken.length() != 64 || WiFi.status() != WL_CONNECTED || !clockReady() || (endpoint != "/bridge_state.php" && cloudBackoffUntil && (int32_t)(millis() - cloudBackoffUntil) < 0)) return -1;
  WiFiClientSecure secure; secure.setCACert(HEATSYNC_CONTROL_CA); secure.setHandshakeTimeout(5);
  HTTPClient http; http.setTimeout(8000); http.setConnectTimeout(3500); http.setReuse(false);
  if (!http.begin(secure, serviceURL + endpoint)) return -1;
  http.addHeader("X-HeatSync-Token", deviceToken); http.addHeader("X-HeatSync-Heater", heaterID);
  http.addHeader("Cache-Control", "no-store");
  int code;
  if (file) { http.addHeader("Content-Type", "application/json"); code = http.sendRequest("POST", file, file->size()); }
  else if (post.length()) { http.addHeader("Content-Type", "application/json"); code = http.POST(post); }
  else code = http.GET();
  if (code > 0 && !file) body = http.getString();
  http.end();
  if (code == 401 || code == 402 || code == 403) cloudBackoffUntil = millis() + 60000;
  return code;
}
