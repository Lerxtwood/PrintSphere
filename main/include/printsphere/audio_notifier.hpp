#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "esp_err.h"

namespace printsphere {

// Lightweight buzzer-style audio notifier driving the on-board ES8311 codec via
// `bsp_audio_codec_speaker_init()`. Renders short square-wave tones / melodies
// from a dedicated FreeRTOS task; never blocks the caller.
//
// Triggering events is fire-and-forget: when the worker is busy or the
// notifier is muted/disabled the request is dropped silently.
class AudioNotifier {
 public:
  enum class Event : uint8_t {
    kPrintStarted,    // idle/preparing -> printing
    kPrintFinished,   // printing -> finished (Tada melody)
    kPrintError,      // has_error edge / new print_error_code
    kHmsAlert,        // new HMS warning code observed
    kPrintPaused,     // printing -> paused
    kFilamentChange,  // filament change / runout stage
    kReconnect,       // optional: cloud/local MQTT recovered after failures
    kClick,           // optional UI click feedback
  };

  static constexpr uint8_t kEventCount = 8;

  AudioNotifier();
  AudioNotifier(const AudioNotifier&) = delete;
  AudioNotifier& operator=(const AudioNotifier&) = delete;

  // Initialises the codec, allocates DMA buffers and spawns the worker task.
  // Safe to call multiple times — subsequent calls are no-ops.
  esp_err_t initialize();

  // Enqueue a notification. Drops silently if disabled, muted, or the queue
  // is full.
  void play(Event event);

  // Master enable/disable (persisted by caller via NVS). When disabled all
  // play() requests are dropped immediately.
  void set_enabled(bool enabled);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Volume in 0..100. Applied as linear amplitude scaling on top of the
  // codec hardware gain.
  void set_volume_percent(int volume);
  int volume_percent() const { return volume_pct_.load(std::memory_order_relaxed); }

  // Quiet-hours gate for normal notification sounds. Test sounds intentionally
  // bypass this via the worker's force flag.
  void set_quiet_hours(bool enabled, uint16_t start_minute, uint16_t end_minute);

  // Plays a short test tone — used by the web portal "Test" button.
  void play_test();

  // Plays a specific event, bypassing the master and per-event enable gates.
  // Used by the web portal per-event test buttons.
  void play_test_event(Event event);

  // Per-event enable/disable (finer gate below the master toggle).
  // Defaults: first 5 events (Print Started/Finished/Error/HMS/Paused) and
  // Click on; Filament Change and Reconnect default to off.
  void set_event_enabled(Event event, bool enabled);
  bool event_enabled(Event event) const;

  // Replace the built-in melody for one event with a custom PCM blob
  // (16 kHz, 16-bit signed mono samples). An empty vector reverts to the
  // built-in tone.
  void set_event_pcm(Event event, std::vector<int16_t> samples);
  void clear_event_pcm(Event event);
  bool has_event_pcm(Event event) const;

 private:
  std::atomic<bool> enabled_{true};
  std::atomic<int> volume_pct_{60};
  std::atomic<bool> quiet_enabled_{true};
  std::atomic<int> quiet_start_min_{21 * 60};
  std::atomic<int> quiet_end_min_{8 * 60};
  std::atomic<bool> initialized_{false};
  std::atomic<bool> event_enabled_[kEventCount];
  std::mutex pcm_mutex_;
  std::vector<int16_t> custom_pcm_[kEventCount];
};

}  // namespace printsphere
