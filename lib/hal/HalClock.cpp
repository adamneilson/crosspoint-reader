#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)
//   0x03: Day of week (1-7, any consistent mapping)
//   0x04: Date    (1-31)
//   0x05: Month   (1-12, bit 7 = century flag)
//   0x06: Year    (00-99, interpreted as 2000-2099)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

// Days since the Unix epoch for a civil UTC date (Howard Hinnant's algorithm).
// Avoids timegm (not portable) and mktime (depends on the TZ environment).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);

  restoreSystemTimeFromRTC();
}

void HalClock::restoreSystemTimeFromRTC() {
  // Only when the system clock is implausible (fresh power-on); never clobber
  // a clock that is already valid.
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  if (t.tm_year + 1900 >= 2024) {
    return;
  }

  // Read registers 0x00-0x06: ss mm hh dow date month year (UTC).
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)7);
  if (Wire.available() < 7) {
    return;
  }
  const uint8_t ss = bcdToDec(Wire.read() & 0x7F);
  const uint8_t mm = bcdToDec(Wire.read() & 0x7F);
  const uint8_t rawHour = Wire.read();
  Wire.read();  // day of week — unused
  const uint8_t day = bcdToDec(Wire.read() & 0x3F);
  const uint8_t month = bcdToDec(Wire.read() & 0x1F);
  const int year = 2000 + bcdToDec(Wire.read());
  const uint8_t hh = (rawHour & 0x40) ? 0 : bcdToDec(rawHour & 0x3F);  // 12h mode never written by us

  // Unset RTCs read as 2000-01-01 (the DS3231 power-on default); only trust a
  // date that an NTP sync must have written.
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) {
    LOG_INF("CLK", "RTC date not set (%04d-%02u-%02u); system clock left as-is", year, month, day);
    return;
  }

  struct timeval tv = {};
  tv.tv_sec = daysFromCivil(year, month, day) * 86400 + hh * 3600 + mm * 60 + ss;
  settimeofday(&tv, nullptr);
  LOG_INF("CLK", "System clock restored from RTC: %04d-%02u-%02u %02u:%02u:%02u UTC", year, month, day, hh, mm, ss);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(const struct tm& utc) {
  assert(utc.tm_hour < 24);
  assert(utc.tm_min < 60);
  assert(utc.tm_sec < 61);  // leap second
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);                                   // Start at register 0x00
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_sec % 60)));  // 0x00: Seconds
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_min)));       // 0x01: Minutes
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_hour)));      // 0x02: Hours (24h mode, bit 6 = 0)
  // Full calendar too: the X3 powers the MCU off completely during deep sleep,
  // so the DS3231 is the only date source available at wake (see
  // restoreSystemTimeFromRTC). tm_wday is 0-6; DS3231 wants 1-7.
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_wday + 1)));             // 0x03: Day of week
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_mday)));                 // 0x04: Date
  Wire.write(decToBcd(static_cast<uint8_t>(utc.tm_mon + 1)));              // 0x05: Month
  Wire.write(decToBcd(static_cast<uint8_t>((utc.tm_year + 1900) % 100)));  // 0x06: Year (base 2000)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = static_cast<uint8_t>(utc.tm_hour);
  _cachedMinute = static_cast<uint8_t>(utc.tm_min);
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (writeTimeToRTC(timeinfo)) {
        LOG_INF("CLK", "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
