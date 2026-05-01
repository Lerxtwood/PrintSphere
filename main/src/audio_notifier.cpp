#include "printsphere/audio_notifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace printsphere {

namespace {

constexpr const char* kTag = "printsphere.audio";

// Sample rate kept low to minimise CPU and DMA pressure — a square wave only
// needs to clear Nyquist for the highest fundamental we play (~1500 Hz).
constexpr int kSampleRate = 16000;
constexpr int kAttackReleaseSamples = 80;  // 5 ms fade — kills click artefacts
constexpr int kQueueDepth = 4;

struct Note {
  uint16_t frequency_hz;  // 0 = silence (rest)
  uint16_t duration_ms;
};

struct Melody {
  const Note* notes;
  uint8_t count;
};

// ----- Pre-built melodies ---------------------------------------------------

constexpr Note kPrintStartedNotes[] = {
    {523, 90},   // C5
    {659, 90},   // E5
    {784, 140},  // G5
};

constexpr Note kPrintFinishedNotes[] = {
    {523, 110}, {659, 110}, {784, 110}, {1047, 220}, {0, 80}, {784, 90}, {1047, 320},
};

constexpr Note kPrintErrorNotes[] = {
    {220, 220}, {0, 80}, {220, 220}, {0, 80}, {165, 360},
};

constexpr Note kHmsAlertNotes[] = {
    {1000, 80},
};

constexpr Note kPrintPausedNotes[] = {
    {659, 100}, {0, 60}, {659, 100},
};

constexpr Note kFilamentChangeNotes[] = {
    {784, 110}, {988, 110}, {784, 110}, {988, 200},
};

constexpr Note kReconnectNotes[] = {
    {600, 60}, {0, 40}, {800, 90},
};

constexpr Note kClickNotes[] = {
    {2000, 8},
};

constexpr Melody melody_for(AudioNotifier::Event event) {
  switch (event) {
    case AudioNotifier::Event::kPrintStarted:
      return {kPrintStartedNotes,
              sizeof(kPrintStartedNotes) / sizeof(kPrintStartedNotes[0])};
    case AudioNotifier::Event::kPrintFinished:
      return {kPrintFinishedNotes,
              sizeof(kPrintFinishedNotes) / sizeof(kPrintFinishedNotes[0])};
    case AudioNotifier::Event::kPrintError:
      return {kPrintErrorNotes,
              sizeof(kPrintErrorNotes) / sizeof(kPrintErrorNotes[0])};
    case AudioNotifier::Event::kHmsAlert:
      return {kHmsAlertNotes,
              sizeof(kHmsAlertNotes) / sizeof(kHmsAlertNotes[0])};
    case AudioNotifier::Event::kPrintPaused:
      return {kPrintPausedNotes,
              sizeof(kPrintPausedNotes) / sizeof(kPrintPausedNotes[0])};
    case AudioNotifier::Event::kFilamentChange:
      return {kFilamentChangeNotes,
              sizeof(kFilamentChangeNotes) / sizeof(kFilamentChangeNotes[0])};
    case AudioNotifier::Event::kReconnect:
      return {kReconnectNotes,
              sizeof(kReconnectNotes) / sizeof(kReconnectNotes[0])};
    case AudioNotifier::Event::kClick:
      return {kClickNotes, sizeof(kClickNotes) / sizeof(kClickNotes[0])};
  }
  return {kHmsAlertNotes, sizeof(kHmsAlertNotes) / sizeof(kHmsAlertNotes[0])};
}

// Worker-owned state. Kept in a single anonymous namespace because there is
// only ever one notifier instance for the whole firmware (matches the rest of
// printsphere's "one-of-everything" design).

esp_codec_dev_handle_t g_codec = nullptr;
QueueHandle_t g_queue = nullptr;
std::atomic<int>* g_volume_ptr = nullptr;
std::atomic<bool>* g_enabled_ptr = nullptr;

void render_square_tone(uint16_t freq_hz, uint16_t duration_ms, int volume_pct,
                        std::vector<int16_t>& buffer) {
  const int total_samples = std::max(1, kSampleRate * duration_ms / 1000);
  buffer.resize(static_cast<size_t>(total_samples));
  if (freq_hz == 0 || volume_pct <= 0) {
    std::fill(buffer.begin(), buffer.end(), int16_t{0});
    return;
  }
  // Cap to safe headroom. The ES8311 + on-board PA gets unpleasantly loud at
  // full scale, and square waves clip the smoothing filter even harder than
  // sine, so we never exceed ~50 % of int16 range.
  const int volume = std::clamp(volume_pct, 0, 100);
  const float amplitude = 0.50f * static_cast<float>(volume) / 100.0f;
  const int16_t peak = static_cast<int16_t>(amplitude * 32767.0f);
  const int half_period_samples = kSampleRate / (2 * std::max<int>(1, freq_hz));
  if (half_period_samples <= 0) {
    std::fill(buffer.begin(), buffer.end(), int16_t{0});
    return;
  }

  bool high = true;
  int phase = 0;
  for (int i = 0; i < total_samples; ++i) {
    int16_t value = high ? peak : static_cast<int16_t>(-peak);

    // Linear attack + release envelope to suppress audible clicks at the
    // start/end of every note.
    if (i < kAttackReleaseSamples) {
      value = static_cast<int16_t>(static_cast<int32_t>(value) * i /
                                    kAttackReleaseSamples);
    } else if (i > total_samples - kAttackReleaseSamples) {
      const int remaining = total_samples - i;
      value = static_cast<int16_t>(static_cast<int32_t>(value) * remaining /
                                    kAttackReleaseSamples);
    }
    buffer[i] = value;

    if (++phase >= half_period_samples) {
      phase = 0;
      high = !high;
    }
  }
}

void worker_task(void*) {
  std::vector<int16_t> render_buffer;
  render_buffer.reserve(kSampleRate / 2);  // up to 0.5 s notes

  AudioNotifier::Event event;
  while (true) {
    if (xQueueReceive(g_queue, &event, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (g_enabled_ptr == nullptr || !g_enabled_ptr->load(std::memory_order_relaxed)) {
      continue;
    }
    if (g_codec == nullptr) {
      continue;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.channel_mask = 0;
    fs.sample_rate = kSampleRate;
    if (esp_codec_dev_open(g_codec, &fs) != ESP_OK) {
      ESP_LOGW(kTag, "codec_dev_open failed");
      continue;
    }
    esp_codec_dev_set_out_vol(g_codec, 75.0f);

    const Melody melody = melody_for(event);
    const int volume = g_volume_ptr != nullptr
                            ? g_volume_ptr->load(std::memory_order_relaxed)
                            : 60;
    for (uint8_t i = 0; i < melody.count; ++i) {
      const Note& note = melody.notes[i];
      render_square_tone(note.frequency_hz, note.duration_ms, volume, render_buffer);
      const int bytes = static_cast<int>(render_buffer.size() * sizeof(int16_t));
      if (bytes > 0) {
        esp_codec_dev_write(g_codec, render_buffer.data(), bytes);
      }
    }

    esp_codec_dev_close(g_codec);
  }
}

}  // namespace

AudioNotifier::AudioNotifier() = default;

esp_err_t AudioNotifier::initialize() {
  if (initialized_.load()) {
    return ESP_OK;
  }

  g_codec = bsp_audio_codec_speaker_init();
  if (g_codec == nullptr) {
    ESP_LOGE(kTag, "bsp_audio_codec_speaker_init failed");
    return ESP_FAIL;
  }

  g_queue = xQueueCreate(kQueueDepth, sizeof(Event));
  if (g_queue == nullptr) {
    ESP_LOGE(kTag, "queue alloc failed");
    return ESP_ERR_NO_MEM;
  }

  g_volume_ptr = &volume_pct_;
  g_enabled_ptr = &enabled_;

  // Stack: square-wave rendering is light, but esp_codec_dev_write goes through
  // I2S DMA + codec driver — 4 kB is comfortable, 3 kB cuts close.
  TaskHandle_t handle = nullptr;
  const BaseType_t ok =
      xTaskCreatePinnedToCore(&worker_task, "audio_notif", 4096, nullptr,
                              tskIDLE_PRIORITY + 3, &handle, 0);
  if (ok != pdPASS) {
    ESP_LOGE(kTag, "task create failed");
    return ESP_ERR_NO_MEM;
  }

  initialized_.store(true);
  ESP_LOGI(kTag, "audio notifier ready (rate=%d Hz, vol=%d%%, enabled=%d)",
           kSampleRate, volume_pct_.load(), enabled_.load() ? 1 : 0);
  return ESP_OK;
}

void AudioNotifier::play(Event event) {
  if (!initialized_.load(std::memory_order_relaxed)) {
    return;
  }
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_queue == nullptr) {
    return;
  }
  // Non-blocking — drop the event if the worker is still rendering.
  xQueueSend(g_queue, &event, 0);
}

void AudioNotifier::play_test() {
  // play_test() is invoked from the web portal even when enabled_ is false
  // (that's the whole point of the "Test" button), so we bypass the gate but
  // still respect initialization.
  if (!initialized_.load(std::memory_order_relaxed) || g_queue == nullptr) {
    return;
  }
  // Briefly flip enabled_ for the duration of the queued event so the worker
  // doesn't drop it. Restoring it before send is harmless because the worker
  // re-checks enabled_ on dequeue and the queue is FIFO single-consumer.
  const bool was_enabled = enabled_.exchange(true, std::memory_order_acq_rel);
  const Event event = Event::kPrintStarted;
  xQueueSend(g_queue, &event, 0);
  // Restore on the next tick; if the worker has already started rendering the
  // brief `true` window let it finish — it will not get re-triggered.
  enabled_.store(was_enabled, std::memory_order_release);
}

void AudioNotifier::set_enabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
}

void AudioNotifier::set_volume_percent(int volume) {
  volume_pct_.store(std::clamp(volume, 0, 100), std::memory_order_relaxed);
}

}  // namespace printsphere
