#pragma once
#include <string>

#include "activities/Activity.h"

/**
 * Boot-time activity that re-downloads the designated OPDS book (see
 * AutoFetchStore) without user interaction, then silent-restarts into the
 * normal boot destination (reader if the user slept from the reader,
 * otherwise home).
 *
 * Guardrails:
 *  - Skippable/cancellable: Back skips instantly while connecting; during the
 *    download the progress callback pumps input and sets the downloader's
 *    cancel flag, so a stuck transfer can be aborted mid-flight.
 *  - Crash/data-safe: downloads to a temp file and swaps it over the
 *    destination only on success; a failed fetch leaves the previous book
 *    untouched.
 *  - Position-safe: reader progress (progress.bin) is captured before the
 *    book's cache is cleared and restored after the swap.
 *  - Battery-safe: the gate in main.cpp launches this at most once per
 *    calendar day and only when a saved WiFi network and target exist. The
 *    throttle is stamped in onEnter(), BEFORE any network work, so no failure
 *    mode can retry-loop. WiFi connect has a hard deadline, and every exit
 *    path tears WiFi down via the standard silent-restart.
 */
class AutoFetchActivity final : public Activity {
 public:
  enum class State { CONNECTING, DOWNLOADING, TEARDOWN };

  explicit AutoFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AutoFetch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  State state = State::CONNECTING;
  unsigned long connectDeadline = 0;
  unsigned long lastCancelPoll = 0;
  bool cancelFlag = false;
  std::string statusText;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  void beginWifiConnect();
  void maybeSyncClock();
  void runFetch();  // blocking download + swap + progress restore
  void teardownAndRestart(bool success);
  bool preventAutoSleep() override { return true; }
};
