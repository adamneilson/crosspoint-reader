#include "AutoFetchStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"

AutoFetchStore AutoFetchStore::instance;

namespace {
constexpr char AUTOFETCH_FILE_JSON[] = "/.crosspoint/autofetch.json";
}  // namespace

bool AutoFetchStore::loadFromFile() {
  if (!Storage.exists(AUTOFETCH_FILE_JSON)) {
    return false;
  }
  const String json = Storage.readFile(AUTOFETCH_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("AFS", "Failed to parse autofetch.json");
    return false;
  }
  url = doc["url"] | "";
  filename = doc["filename"] | "";
  username = doc["username"] | "";
  password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "");
  lastFetchYmd = doc["lastFetchYmd"] | "";
  return true;
}

bool AutoFetchStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["url"] = url;
  doc["filename"] = filename;
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["lastFetchYmd"] = lastFetchYmd;
  String json;
  serializeJson(doc, json);
  return Storage.writeFile(AUTOFETCH_FILE_JSON, json);
}

void AutoFetchStore::setTarget(const std::string& url, const std::string& filename, const std::string& username,
                               const std::string& password) {
  this->url = url;
  this->filename = filename;
  this->username = username;
  this->password = password;
  lastFetchYmd = todayYmd();
  if (!saveToFile()) {
    LOG_ERR("AFS", "Failed to save auto-fetch target");
  }
}

void AutoFetchStore::markAttempt(const std::string& ymd) {
  lastFetchYmd = ymd;
  if (!saveToFile()) {
    LOG_ERR("AFS", "Failed to save auto-fetch throttle stamp");
  }
}

std::string AutoFetchStore::todayYmd() {
  time_t now = time(nullptr);
  // Local date using the user's configured UTC offset (quarter-hours, biased by 48)
  now += (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60;
  struct tm t;
  gmtime_r(&now, &t);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}
