# Repository Guidelines

## Structure

`main/app_main.c` adapts ESP-IDF Wi-Fi and the UART command/session loop.
`main/gps_service.c` owns the independent GPS UART task; `status_led.c` drives RMT.
`main/ap_memory.c` owns AP/index/observation PSRAM allocations.
`main/radio_capture.c` pairs passive beacon metadata with `core/gps_history.c`.
Scan results and the bounded reception cache also use explicit PSRAM allocations.
`main/core/` is portable C for NMEA, CRC framing, delivery, WiGLE and wardriving.
`tests/` contains host C/Python checks. `tools/` contains protocol and hardware tools.
`docs/FLIPPER_INTEGRATION.md` is the authoritative handoff to the separate FAP project.
Do not implement the Flipper application in this repository.

## Build and validation

Use ESP-IDF 6.0.0, target esp32c5; after activating the official environment run
`idf.py build`. GPIO and limits are configured with `idf.py menuconfig`.
Host checks: `cmake -S tests -B build-host -DFEB_SANITIZERS=ON`,
`cmake --build build-host`, `ctest --test-dir build-host --output-on-failure`.
Do not claim hardware or FAP/SD validation from host checks or a firmware build.
Do not flash or reset connected hardware unless requested.

## Style and design

Use C, four spaces, snake_case, `_t` types and uppercase constants. `.clang-format`
is the reproducible formatter configuration. Keep core modules independent of IDF.
Use bounded memory, explicit overflow handling and immutable queued observations.
Target module: ESP32-C5-WROOM-1U-N8R8, LCSC C51950748 (8 MiB flash/PSRAM).
AP records, index and observation queue use explicit MALLOC_CAP_SPIRAM allocations;
UART/GPS buffers and task stacks stay internal. Missing PSRAM must disable START
with diagnostics, never silently reduce the requested capacity. GPIO15 is reserved
for the module PSRAM; keep existing GPIO defaults.
BSSID is the key; never merge by SSID. Preserve existing GPIO defaults and FBR1
legacy compatibility. New protocol behavior must be documented for both endpoints.
Changes to thresholds compare against the last accepted observation, not a raw hit.
Driving defaults to START_SCAN with no extra gap (0 ms), passive 120 ms dwell.
Do not trade reception coverage for a misleading subsecond full-sweep claim.
Only use GPS solutions published at or before the received frame timestamp.
SDK results without matching metadata stay unlocalized and carry RX_UNVERIFIED.

## Testing and changes

Add meaningful regression checks for behavior changes, including invalid inputs,
queue pressure, retransmission and GPS validity. State simulations separately from
physical device tests. Use concise imperative commit subjects; include validation
and remaining hardware checks in reviews. Keep credentials and machine-specific
configuration out of committed files; never erase NVS automatically.
