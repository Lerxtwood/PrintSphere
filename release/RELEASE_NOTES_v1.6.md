# PrintSphere v1.6

Release covering the final v1.6 line after the v1.6 RC cycle.

## Release Scope

- **Required base flash**: v1.6 changes the partition table. Devices on v1.5.x or older need one full USB/WebSerial flash before OTA updates are used again.
- **Toolchain target**: ESP-IDF v5.5.4, LVGL v9.5.0.
- **Display adapter**: `espressif/esp_lvgl_adapter` v0.6.0. PrintSphere uses the official TE-vsync and buffer-switch fixes, plus one narrow local GPIO-TE TX-done timeout safety patch.

## Major Features

- **Sound notifications**: Eight event sounds are available: Print Started, Print Finished, Print Error, HMS Alert, Print Paused, Filament Change, Reconnect, and Click.
- **Per-event sound control**: Each sound can be enabled/disabled, tested, cleared, or replaced from Web Config.
- **Custom WAV upload**: Web Config accepts mono 16-bit 16 kHz WAV files and stores the converted PCM on LittleFS.
- **LittleFS sound partition**: Custom sounds use a dedicated LittleFS partition instead of NVS blobs. NVS was expanded to 1 MB.
- **Multi-AMS pages**: The UI supports up to four AMS pages with per-unit tray rendering and clearer page labels.
- **HMS tray indicators**: AMS/HMS error codes are resolved down to affected tray slots where possible and highlighted in the tray UI.
- **P2S/H2 protocol coverage**: V2 protocol fields such as `snow` and `vir_slot` are parsed for newer printer families.

## Major Fixes

- **Display lock storms reduced**: Preview and printer-select pages no longer run continuous label/dot animations that can starve LVGL locking under load.
- **Preview image redraw fixed**: The cover image source is no longer re-applied every snapshot once cached, avoiding repeated large image invalidations.
- **Display deadlock hardening**: esp_lvgl_adapter v0.6.0 brings the official bounded TE-vsync wait, stale DMA completion cleanup, and buffer-switch synchronization updates. PrintSphere still bounds the remaining GPIO-TE TX-done wait locally so a missed panel IO completion cannot hold the LVGL worker forever.
- **Local MQTT reconnect discipline**: The TCP probe is advisory, esp-mqtt is given patient connect/reconnect windows, the MQTT client is not rebuilt aggressively while esp-mqtt can reconnect internally, and auth errors are latched instead of causing rebuild loops.
- **Local MQTT TLS diagnostics**: Transport errors now include ESP, TLS, verify, and socket error detail to separate network timeouts from certificate/auth failures.
- **Short sounds audible**: Click and HMS built-in sounds were lengthened, and silent/too-short custom PCM now falls back to the built-in sound.
- **Sound upload validation**: Web Config rejects WAV uploads that are too short or effectively silent.
- **FatFS compatibility on IDF 5.5.4**: Long filename support is enabled to satisfy the IDF 5.5 FatFS/exFAT configuration.

## Internal Changes

- `main/idf_component.yml` now targets ESP-IDF `>=5.5.4`.
- `components/esp32_s3_touch_amoled_1_75/idf_component.yml` pins `espressif/esp_lvgl_adapter` to `0.6.0`.
- `joltwallet/littlefs` is added for `/sounds` storage.
- `tools/patches/apply_adapter_patches.ps1` is retained as a narrow GPIO-TE TX-done timeout safety patch for esp_lvgl_adapter v0.6.0.
- The partition table now contains expanded NVS, two 5 MB OTA slots, and a `sounds` LittleFS partition.

## Known Notes

- esp_lvgl_adapter v0.6.0 fixes the TE-vsync path and improves buffer-switch synchronization, but the upstream GPIO-TE SPI flush still waits on TX-done with `portMAX_DELAY`. PrintSphere carries a local timeout patch for that one remaining path.
- The current release binaries should be regenerated from the final v1.6 tree before tagging/publishing.

## Tested Focus

- ESP-IDF v5.5.4 build environment.
- Waveshare ESP32-S3 Touch AMOLED 1.75.
- P1S local MQTT, cloud MQTT, cover preview, camera page, sound test/upload, and Web Config flows.
