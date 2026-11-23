# Feberis Pro Wardriver

**Feberis Pro** is firmware for passively discovering 2.4 GHz and 5 GHz Wi-Fi
networks and recording their parameters alongside GNSS positions. A Flipper Zero
controls the device over UART. The ESP32 scans, deduplicates BSSIDs, and prepares
observations for storage by the Flipper application.

Current version: **0.3.1**. Target module: **ESP32-C5-WROOM-1U-N8R8**,
LCSC **C51950748**, with 8 MiB flash and 8 MiB PSRAM.

## Features

- Passive scans of the 2.4 GHz band, 5 GHz band, or both, including hidden SSIDs.
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

## Tests

Host tests require a C compiler, CMake, and Python 3.10+; no device is required:

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Version 0.3.1 passed **7/7 host test suites with ASan/UBSan**, **15/15 hardware
acceptance scenarios**, and a 12-cycle memory stability check. Testing covered both
bands, recording modes, GPS, retransmission, recovery, restart, and three UART
baud rates. The [test report](docs/HARDWARE_TEST_2026-11-23_ACCEPTANCE.md) (Polish)
distinguishes physical device results from simulations.

## Documentation

- [Technical documentation and user guide](docs/DOKUMENTACJA.md) — English.
- [UART protocol and Flipper integration](docs/FLIPPER_INTEGRATION.md) — Polish.
- [Architecture, data model, and SMART algorithm](docs/ARCHITECTURE.md) — Polish.
- [Test matrix and validation](docs/VALIDATION.md) — Polish.
- [Example output files](docs/examples).

## Limitations

Positions are assigned at scan completion, so movement introduces an error related
to the time spent scanning channels. PSRAM is volatile: restarting clears pending
records and aggregates. Reaching the AP capacity limit is reported explicitly.
Observations without a valid location are excluded from WiGLE export.

Actual microSD/FAP storage, GNSS testing while driving, extended operation, and
WiGLE server import require further validation. The FAP application is maintained
in its separate project.
