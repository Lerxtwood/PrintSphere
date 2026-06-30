#include "printsphere/pmu.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.pmu";
constexpr adc_unit_t kBatteryAdcUnit = ADC_UNIT_1;
constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_3;
constexpr adc_atten_t kBatteryAdcAtten = ADC_ATTEN_DB_12;
constexpr float kBatteryDividerScale = 3.0f;
constexpr float kBatteryVoltageOffset = 0.980952f;
constexpr float kBatteryPresentMinVoltage = 3.20f;
// On the 2.8C board the ADC node reads about 4.27V on USB-only power with no
// battery attached. Treat anything above a realistic Li-Ion full-charge window
// as "not a real battery" to avoid a permanent fake 100% overlay.
constexpr float kBatteryPresentMaxVoltage = 4.18f;
constexpr float kUsbOnlyArtifactVoltage = 4.24f;
constexpr uint32_t kDiagLogIntervalMs = 5000U;
constexpr uint32_t kUsbArtifactConfirmSamples = 1U;
constexpr uint32_t kBatteryPresentConfirmSamples = 12U;
constexpr uint32_t kNoBatteryConfirmSamples = 6U;

enum PowerSenseState {
  kSenseUnknown = 0,
  kSenseUsbArtifact = 1,
  kSenseBatteryPresent = 2,
  kSenseNoBattery = 3,
};

adc_oneshot_unit_handle_t s_adc_handle = nullptr;
adc_cali_handle_t s_adc_cali_handle = nullptr;
bool s_adc_cali_enabled = false;
uint32_t s_last_diag_log_ms = 0;
int s_last_diag_state = -1;
PowerSenseState s_stable_state = kSenseUnknown;
uint32_t s_usb_artifact_count = 0;
uint32_t s_battery_present_count = 0;
uint32_t s_no_battery_count = 0;

uint8_t battery_percent_from_voltage(float voltage) {
  if (voltage <= 3.30f) {
    return 0;
  }
  if (voltage >= 4.20f) {
    return 100;
  }
  const float normalized = (voltage - 3.30f) / (4.20f - 3.30f);
  return static_cast<uint8_t>(normalized * 100.0f + 0.5f);
}

const char* power_sense_state_name(int state) {
  switch (state) {
    case kSenseUsbArtifact:
      return "usb-artifact";
    case kSenseBatteryPresent:
      return "battery";
    case kSenseNoBattery:
      return "no-battery";
    case kSenseUnknown:
    default:
      return "unknown";
  }
}

void maybe_log_power_diag(int raw, int millivolts, float battery_voltage, int state, int inst_state,
                          const PowerSnapshot& snapshot) {
  const uint32_t now_ms = esp_log_timestamp();
  if (state != s_last_diag_state || (now_ms - s_last_diag_log_ms) >= kDiagLogIntervalMs) {
    ESP_LOGI(kTag,
             "PMU diag: raw=%d adc_mv=%d batt_v=%.3f state=%s inst=%s battery_present=%d usb_present=%d charging=%d pct=%u",
             raw, millivolts, battery_voltage, power_sense_state_name(state),
             power_sense_state_name(inst_state),
             snapshot.battery_present ? 1 : 0, snapshot.usb_present ? 1 : 0,
             snapshot.charging ? 1 : 0, snapshot.battery_percent);
    s_last_diag_log_ms = now_ms;
    s_last_diag_state = state;
  }
}

PowerSenseState instantaneous_power_state(float battery_voltage) {
  if (battery_voltage >= kUsbOnlyArtifactVoltage) {
    return kSenseUsbArtifact;
  }
  if (battery_voltage >= kBatteryPresentMinVoltage &&
      battery_voltage <= kBatteryPresentMaxVoltage) {
    return kSenseBatteryPresent;
  }
  return kSenseNoBattery;
}

PowerSenseState resolve_stable_power_state(PowerSenseState inst_state) {
  if (inst_state == kSenseUsbArtifact) {
    s_usb_artifact_count++;
    s_battery_present_count = 0;
    s_no_battery_count = 0;
    if (s_usb_artifact_count >= kUsbArtifactConfirmSamples) {
      s_stable_state = kSenseUsbArtifact;
    }
    return s_stable_state;
  }

  if (inst_state == kSenseBatteryPresent) {
    s_battery_present_count++;
    s_usb_artifact_count = 0;
    s_no_battery_count = 0;
    if (s_battery_present_count >= kBatteryPresentConfirmSamples) {
      s_stable_state = kSenseBatteryPresent;
    }
    return s_stable_state;
  }

  s_no_battery_count++;
  s_usb_artifact_count = 0;
  s_battery_present_count = 0;
  if (s_no_battery_count >= kNoBatteryConfirmSamples) {
    s_stable_state = kSenseNoBattery;
  }
  return s_stable_state;
}

}  // namespace

esp_err_t PmuManager::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  const adc_oneshot_unit_init_cfg_t unit_config = {
      .unit_id = kBatteryAdcUnit,
      .clk_src = static_cast<adc_oneshot_clk_src_t>(0),
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  if (adc_oneshot_new_unit(&unit_config, &s_adc_handle) != ESP_OK) {
    ESP_LOGW(kTag, "ADC unit init failed; battery telemetry disabled");
    return ESP_OK;
  }

  const adc_oneshot_chan_cfg_t channel_config = {
      .atten = kBatteryAdcAtten,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_oneshot_config_channel(s_adc_handle, kBatteryAdcChannel, &channel_config) != ESP_OK) {
    ESP_LOGW(kTag, "ADC channel config failed; battery telemetry disabled");
    return ESP_OK;
  }

  const adc_cali_curve_fitting_config_t cali_config = {
      .unit_id = kBatteryAdcUnit,
      .chan = kBatteryAdcChannel,
      .atten = kBatteryAdcAtten,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle) == ESP_OK) {
    s_adc_cali_enabled = true;
  }

  initialized_ = true;
  ESP_LOGI(kTag, "Battery ADC ready");
  return ESP_OK;
}

PowerSnapshot PmuManager::sample() const {
  PowerSnapshot snapshot;
  if (!initialized_ || s_adc_handle == nullptr) {
    return snapshot;
  }

  int raw = 0;
  if (adc_oneshot_read(s_adc_handle, kBatteryAdcChannel, &raw) != ESP_OK) {
    return snapshot;
  }

  int millivolts = 0;
  if (s_adc_cali_enabled) {
    if (adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &millivolts) != ESP_OK) {
      return snapshot;
    }
  } else {
    millivolts = raw;
  }

  const float battery_voltage =
      ((static_cast<float>(millivolts) * kBatteryDividerScale) / 1000.0f) / kBatteryVoltageOffset;
  const PowerSenseState inst_state = instantaneous_power_state(battery_voltage);
  const PowerSenseState stable_state = resolve_stable_power_state(inst_state);

  snapshot.available = true;
  snapshot.battery_present = stable_state == kSenseBatteryPresent;
  snapshot.usb_present = stable_state == kSenseUsbArtifact;
  snapshot.charging = false;
  snapshot.temperature_c = 0.0f;
  snapshot.battery_percent =
      snapshot.battery_present ? battery_percent_from_voltage(battery_voltage) : 0;
  maybe_log_power_diag(raw, millivolts, battery_voltage, stable_state, inst_state, snapshot);

  return snapshot;
}

}  // namespace printsphere
