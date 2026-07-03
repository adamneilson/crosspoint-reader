#pragma once
#include <string>

/**
 * Persists the daily auto-fetch target and throttle state on the SD card
 * (/.crosspoint/autofetch.json).
 *
 * The target is the last book downloaded from an OPDS server. When the
 * autoFetchDaily setting is enabled, boot re-downloads it at most once per
 * calendar day (see AutoFetchActivity and the gate in main.cpp).
 *
 * Passwords are XOR-obfuscated with the device MAC + base64 on disk,
 * matching OpdsServerStore.
 */
class AutoFetchStore {
 private:
  static AutoFetchStore instance;
  AutoFetchStore() = default;

 public:
  std::string url;       // fully-resolved download URL
  std::string filename;  // destination path on SD, e.g. "/Author - Title.epub"
  std::string username;
  std::string password;      // plaintext in memory, obfuscated on disk
  std::string lastFetchYmd;  // "YYYY-MM-DD" of the last attempt (throttle stamp)

  AutoFetchStore(const AutoFetchStore&) = delete;
  AutoFetchStore& operator=(const AutoFetchStore&) = delete;
  static AutoFetchStore& getInstance() { return instance; }

  bool hasTarget() const { return !url.empty() && !filename.empty(); }

  bool loadFromFile();
  bool saveToFile() const;

  // Remember the book to re-download daily. Called after every successful OPDS
  // download. Also stamps the throttle with today: a manual download counts as
  // today's fetch, so the next wake won't immediately re-download the same file.
  void setTarget(const std::string& url, const std::string& filename, const std::string& username,
                 const std::string& password);

  // Stamp the throttle date and persist immediately. Runs BEFORE a fetch begins
  // so a crash or restart mid-fetch can never boot-loop into another attempt.
  void markAttempt(const std::string& ymd);

  // Today's local date as "YYYY-MM-DD", using the user's configured UTC offset.
  // System time survives deep sleep on the ESP32 (RTC clock domain); after a
  // full power loss it restarts from the epoch, which still throttles correctly
  // (a constant, wrong date) until NTP corrects it during the next fetch.
  static std::string todayYmd();
};

#define AUTOFETCH_STORE AutoFetchStore::getInstance()
