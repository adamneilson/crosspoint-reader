#include "AutoFetchActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <ctime>

#include "AutoFetchStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/reader/ProgressFile.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long CANCEL_POLL_INTERVAL_MS = 250;
}  // namespace

void AutoFetchActivity::onEnter() {
  Activity::onEnter();

  // Stamp the throttle BEFORE any network work: every exit path is a silent
  // restart that re-runs the boot gate, so an unstamped attempt would loop.
  // SD write needs the render lock: SD and display share the SPI bus.
  {
    RenderLock lock(*this);
    AUTOFETCH_STORE.markAttempt(AutoFetchStore::todayYmd());
  }

  statusText = tr(STR_AUTO_FETCH_CONNECTING);
  beginWifiConnect();
  connectDeadline = millis() + CONNECT_TIMEOUT_MS;
  requestUpdate(true);
}

void AutoFetchActivity::beginWifiConnect() {
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    // The boot gate checks this; re-check defensively in case the store changed.
    teardownAndRestart(false);
    return;
  }

  // Same connect mechanics as WifiSelectionActivity::attemptConnection().
  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);  // Abort any in-progress SDK auto-connect and clear NVS-saved SSID
  delay(100);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  const String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());

  if (!cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str());
  }
  LOG_DBG("AFA", "Auto-connecting to %s", cred->ssid.c_str());
}

void AutoFetchActivity::maybeSyncClock() {
  // With a dead or never-synced clock the throttle stamp from onEnter() is
  // wrong; correct it while the network is up anyway. Blocks for up to ~5s.
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  if (t.tm_year + 1900 >= 2024) {
    return;
  }
  if (halClock.isAvailable() && halClock.syncFromNTP()) {
    if (!SETTINGS.clockHasBeenSynced) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.saveToFile();
    }
    // Re-stamp with the corrected date.
    AUTOFETCH_STORE.markAttempt(AutoFetchStore::todayYmd());
    LOG_DBG("AFA", "Clock synced; throttle re-stamped");
  }
}

void AutoFetchActivity::loop() {
  if (state == State::TEARDOWN) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    LOG_DBG("AFA", "Skipped by user");
    teardownAndRestart(false);
    return;
  }

  if (state == State::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      maybeSyncClock();
      state = State::DOWNLOADING;
      statusText = tr(STR_AUTO_FETCH_FETCHING);
      requestUpdate(true);
      runFetch();  // blocking; ends in teardownAndRestart()
    } else if (millis() > connectDeadline || WiFi.status() == WL_CONNECT_FAILED) {
      LOG_DBG("AFA", "WiFi connect failed or timed out; skipping fetch");
      teardownAndRestart(false);
    }
  }
}

void AutoFetchActivity::runFetch() {
  const std::string dest = AUTOFETCH_STORE.filename;
  const std::string tmp = dest + ".part";

  const auto result = HttpDownloader::downloadToFile(
      AUTOFETCH_STORE.url, tmp,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        // Pump input between chunks so Back can cancel a stuck or slow transfer.
        const unsigned long nowMs = millis();
        if (nowMs - lastCancelPoll > CANCEL_POLL_INTERVAL_MS) {
          lastCancelPoll = nowMs;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back)) {
            cancelFlag = true;
          }
          requestUpdate(true);
        }
      },
      &cancelFlag, AUTOFETCH_STORE.username, AUTOFETCH_STORE.password);

  if (result != HttpDownloader::OK) {
    LOG_ERR("AFA", "Fetch failed (%d)", static_cast<int>(result));
    Storage.remove(tmp.c_str());  // downloadToFile removes partials itself; belt and braces
    teardownAndRestart(false);
    return;
  }

  // Capture reader progress: it lives inside the book's cache directory, which
  // must be cleared (the layout cache describes the OLD file's bytes).
  uint8_t progress[6];
  int progressLen = 0;
  {
    const Epub epub(dest, "/.crosspoint");
    HalFile f;
    if (Storage.openFileForRead("AFA", epub.getCachePath() + "/progress.bin", f)) {
      progressLen = f.read(progress, sizeof(progress));
    }
  }
  clearBookCache(dest);

  // Swap the fresh download into place. SdFat's rename won't overwrite, so
  // remove-then-rename: the crash window leaves no book at the path, never a
  // torn one (same tradeoff as ProgressFile::writeAtomic).
  Storage.remove(dest.c_str());
  if (!Storage.rename(tmp.c_str(), dest.c_str())) {
    LOG_ERR("AFA", "Rename into place failed: %s", dest.c_str());
    teardownAndRestart(false);
    return;
  }

  // Restore reading position so the refreshed book reopens where the user left
  // off (the fetch server appends new content after the old, so the saved
  // spine index and page remain valid).
  if (progressLen == 4 || progressLen == 6) {
    const Epub epub(dest, "/.crosspoint");
    epub.setupCacheDir();
    ProgressFile::writeAtomic(epub.getCachePath(), progress, static_cast<size_t>(progressLen));
  }

  LOG_DBG("AFA", "Auto-fetch complete: %s", dest.c_str());
  teardownAndRestart(true);
}

void AutoFetchActivity::teardownAndRestart(const bool success) {
  state = State::TEARDOWN;
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Land where the interrupted boot was headed: back into the open book if the
  // user slept from the reader, otherwise home. Mirrors the routing ladder in
  // setup(); the restart's Silent resume path can never re-enter this activity.
  if (APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty() && APP_STATE.readerActivityLoadCount == 0) {
    silentRestartToReader();
  } else {
    silentRestart();
  }
  (void)success;
}

void AutoFetchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_AUTO_FETCH_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statusText.c_str());

  if (state == State::DOWNLOADING && downloadTotal > 0) {
    GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress, downloadTotal);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_AUTO_FETCH_SKIP_HINT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
