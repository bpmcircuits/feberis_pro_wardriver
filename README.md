# Feberis Pro Wardriver

**Feberis Pro** is firmware for passively discovering 2.4 GHz and 5 GHz Wi-Fi
networks and recording their parameters alongside GNSS positions. A Flipper Zero
controls the device over UART. The ESP32 scans, deduplicates BSSIDs, and prepares
observations for storage by the Flipper application.

Current version: **0.4.0**. Target module: **ESP32-C5-WROOM-1U-N8R8**,
LCSC **C51950748**, with 8 MiB flash and 8 MiB PSRAM.

## Features

- Passive scans of the 2.4 GHz band, 5 GHz band, or both, including hidden SSIDs.
- Continuous scanning by default: no intentional pause between completed cycles.
- Up to **512 AP results per scan**, with bounded batch processing in PSRAM.
- Passive beacon/probe-response timestamps paired with a bounded GNSS history;
  exact SSID bytes, including embedded NUL, are preserved when captured.
- Three recording modes: **SMART**, **FULL**, and **UNIQUE**.
- AP identification by BSSID, RSSI aggregates, and network parameter change detection.
- GPS/GNSS in an independent task: NMEA RMC/GGA, UTC, position, altitude, speed,
  course, satellites, HDOP, and data freshness and consistency checks.
- Default capacity of **10,000 APs** and **2,048 queued observations** in PSRAM.
- **FBR1 revision 2** protocol: CRC-16, sequence numbers, acknowledgments,
  retransmissions, and transport recovery after a disconnection.
- NVS configuration, session statistics, and SRAM/PSRAM diagnostics over UART.
- WiGLE CSV row generation; the export metadata uses **Sapsan** as its brand.

| Mode | Purpose |
| --- | --- |
| SMART | Record first sightings, movement, RSSI/metadata changes, periodic updates, and GPS recovery. |
| FULL | Record every result or sample results at a configurable interval. |
| UNIQUE | Record each BSSID once per session while continuing to update its aggregates. |

## System overview

```text
2.4/5 GHz Wi-Fi ──scan──> ESP32-C5 <──UART1/NMEA── GPS/GNSS
                              │
                         UART0 / FBR1
                              │
                         Flipper Zero
                              │
                         FAP app → microSD
```

This repository contains the **ESP32-C5 firmware, tests, and PC tools**.
The FAP application, its GUI, and microSD storage are developed in a separate
project. The handoff for that project is
[FLIPPER_INTEGRATION.md](docs/FLIPPER_INTEGRATION.md) (currently in Polish).

## Quick start

Requirements: **ESP-IDF 6.0.0**, the ESP32-C5 toolchain, and **CMake ≥3.22**.
Clone the repository and enter its directory:

```sh
git clone https://github.com/bpmcircuits/feberis_pro_wardriver.git
cd feberis_pro_wardriver
```

After activating the ESP-IDF environment:

```sh
idf.py set-target esp32c5
idf.py menuconfig
idf.py build
```

Run `set-target` during initial setup; it may recreate the local `sdkconfig`.
In `menuconfig`, check the GPIO assignments, deployment country, and GNSS baud rate.
Example flashing command through the Flipper USB–UART bridge:

```sh
idf.py -p /dev/cu.usbmodemflip_Tutel1 -b 460800 flash
```

Use the correct port for your computer. The FAP application must release the UART
while flashing. Wiring, PSRAM, and configuration details are in the
[technical documentation](docs/DOKUMENTACJA.md).

## Driving configuration

Use passive scanning, SMART mode, 120 ms channel dwell, and `START_SCAN 0`
(or `START_SCAN` without an argument). The argument is an **extra pause after a
completed batch**, not the duration of a scan. It now accepts 0..60000 ms;
long pauses remain available for diagnostics or stationary use.
The SMART 2000 ms minimum interval limits repeated records of the same BSSID;
it does not slow the radio or delay first detections.

The driving build uses the 240 MHz CPU, a 1 ms scheduler tick, disabled Wi-Fi
modem sleep, a buffered UART transmitter, and ingestion slices capped at 64
results or 2 ms. No scans overlap. The AP table stays at 10,000 entries and the
observation queue at 2,048. `GET_SCAN_METRICS` exposes actual radio time, processing
time, gaps, provenance coverage, and backpressure. See the
[Flipper handoff](docs/FLIPPER_INTEGRATION.md#continuous-driving-profile-in-040)
for the matching application change.

## Tests

Host tests require a C compiler, CMake, and Python 3.10+; no device is required:

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Version 0.4.0 passed **8/8 host suites with ASan/UBSan**, a target firmware build,
and Clang analysis of seven portable modules. The physical acceptance run passed
15 scenarios; one GPS policy-recovery scenario was not run because its initial
fix failed the configured quality threshold. Retransmission, restart, modes,
both bands, and all three UART rates were exercised. A 12-cycle memory test
showed constant free RAM and PSRAM after draining each batch.

Measured local median cycle times improved from **2.99 to 1.70 s** (2.4 GHz) and
**6.52 to 5.09 s** (both bands), comparing 0.3.1 with its 1000 ms minimum gap to
0.4.0 with continuous scanning, at the same 120 ms passive dwell. This measures
cadence, not comparative RF sensitivity or moving coverage. See the
[0.4.0 validation report](docs/HARDWARE_TEST_2026-11-23_CONTINUOUS.md) for results
and physical-test boundaries.

## Documentation

- [Technical documentation and user guide](docs/DOKUMENTACJA.md) — English.
- [UART protocol and Flipper integration](docs/FLIPPER_INTEGRATION.md) — Polish.
- [Architecture, data model, and SMART algorithm](docs/ARCHITECTURE.md) — Polish.
- [0.4.0 tests and scan-cadence measurements](docs/HARDWARE_TEST_2026-11-23_CONTINUOUS.md) — English.
- [Earlier test matrix and validation](docs/VALIDATION.md) — Polish.
- [Example output files](docs/examples).

## Limitations

Positions use the strongest captured beacon/probe response and the most recent
GNSS solution already available at reception. GNSS latency and sample rate still
limit accuracy. SDK results without matching reception metadata are retained as
`NO_GPS` with provenance flag 128. One radio scans channels sequentially: a zero
cycle gap does not make a complete dual-band sweep take less than a second.
PSRAM is volatile: restarting clears pending
records and aggregates. Reaching the AP capacity limit is reported explicitly.
Observations without a valid location are excluded from WiGLE export.

Actual microSD/FAP storage, GNSS testing while driving, extended operation, and
WiGLE server import require further validation. The FAP application is maintained
in its separate project.
