# Firmware 0.4.0 — continuous wardriving validation

Date: 2026-11-23. Target: ESP32-C5-WROOM-1U-N8R8, 8 MiB flash and PSRAM.
The physical board was flashed through the Flipper Zero USB–UART bridge.
The Flipper served as the bridge; the test receiver and its temporary durable
journal ran on the host computer. This does not validate the FAP GUI or microSD.

## Outcome

- ESP-IDF 6.0.0 target build: passed, 802,000 bytes.
- Host ASan/UBSan: 8/8 suites passed.
- Clang static analysis: seven portable core modules, no diagnostics.
- Physical acceptance: 15 scenarios passed; one GPS policy-recovery scenario
  was not run because no qualifying initial fix was available at that point.
- Cadence: all four passive profiles passed; minimum six completed cycles/profile.
- Memory stability: 12 pause/resume/drain cycles passed, without unexpected reset.
- All hardware runs restored the original NVS settings and ended stopped with an
  empty observation queue. No NVS erase was performed.

Final image SHA-256:

```text
4ece850b1a8fbb5ed122c34e129c376d69bc0d35ef17935d68198a31cc810ee9
```

## Acquisition changes verified

`START_SCAN` defaults to zero extra gap. Explicit gaps 0, 50, 999, and 60000 ms
are accepted; negative, overflow, and 60001 ms values are rejected. The long-gap
case checks command acceptance and safe pause/stop, not a minute-long cadence.
Passive dwell stays at 120 ms. SDK scans do not overlap. Processing uses PSRAM
and slices limited to 64 records or 2 ms. The UART read no longer adds a 20 ms
wait per AP. A 4096-byte TX ring decouples normal transmission from acquisition.

The driving build uses 240 MHz, a 1000 Hz tick, a 12 KiB main stack, and disabled
Wi-Fi power saving. The unused 802.15.4 driver is disabled so it does not force
coexistence power management. The 10,000-AP table and 2,048-observation queue are
unchanged. The scan buffer now holds 512 SDK AP results, using 49,152 B PSRAM.
The per-scan capture cache uses 327,680 B PSRAM, with a 256-entry metadata queue
in internal RAM. Existing protocol reply layouts and NVS config fields remain.

The receive-only management callback captures beacon/probe-response metadata.
A per-scan BSSID/channel cache keeps the strongest frame, reception timestamp,
and corresponding historical GPS snapshot together. GPS history never supplies
a fix published after reception. SDK results without matching metadata remain
unlocalized and carry provenance bit 128. They are not silently discarded or
exported as localized WiGLE points. Raw beacons are not transmitted over UART.

## Measured cadence

All profiles use passive 120 ms dwell, the same country/band configuration, and
115200-baud UART with fsync-before-ACK on the host. Periods are measured from
received SCAN_BEGIN events. Gap is the last device-reported completion-to-start
interval. Metadata is matched SDK results / unmatched SDK results for the profile.

| Profile | Requested extra gap, ms | Median period, ms | Last actual gap, ms | Metadata hits/misses |
| --- | ---: | ---: | ---: | ---: |
| 2.4 GHz | 1000 | 2701.55 | 1008 | 58/0 |
| 2.4 GHz | 0 | 1702.49 | 8 | 62/0 |
| Both bands | 1000 | 6091.41 | 1008 | 79/0 |
| Both bands | 0 | 5093.03 | 8 | 89/0 |

The 0.3.1 baseline, with its minimum 1000 ms gap, measured 2989.95 ms for 2.4 GHz
and 6520.13 ms for both bands. Continuous 0.4.0 measured 1702.49 and 5093.03 ms,
respectively: approximately 43% and 22% shorter cycles. New firmware with a
1000 ms gap is included above to separate ingestion improvements from removing
the deliberate delay. The zero-gap profiles reported maximum actual gaps of
8 ms (2.4 GHz) and 10 ms (both). These are local stationary measurements with
roughly a dozen visible networks, not guaranteed worst-case timings, comparative
sensitivity measurements, or proof of market leadership.

All four final profiles had zero metadata misses, metadata queue drops, malformed
captured bodies, cache overflows, scan errors, and observation queue overflows.
The measured main-task stack high-water margin was 4660 B. The SDK scan-results
buffer and reception queue remain bounded even when coverage is incomplete.

## Host edge coverage

The new acquisition suite covers NMEA-to-history integration, invalid checksums
without refreshing fix age, GPS loss/recovery, historical lookup before/after a
fix, no future fixes, ring wrap, stale samples, saturating age arithmetic through
INT64/UINT32 boundaries, and invalid-location propagation.

It checks beacon and probe-response parsing, hidden and 0..32-byte SSIDs with
embedded NUL, truncated/duplicate/oversized/missing SSID IEs, invalid frame flags,
BSSID/RSSI/channel validation, and 30,000 deterministic malformed IE trials.
Cache tests verify paired strongest RSSI/time/GPS, same BSSID on different
channels, stale generations, future samples, capacity pressure, existing-entry
updates when full, generation wrap, and unverified provenance not forcing writes.
Existing suites cover SMART thresholds, metadata changes, GPS policy, 10,000 APs,
the 2,048-entry queue, durable ACK/retry/deduplication, unavailable PSRAM and failed
allocations, mode changes, sequence exhaustion, malformed UART/NMEA, and CSV.

## Physical acceptance and memory

The run checked damaged CRC and oversized UART frames, optional/subsecond scan
gaps, passive 2.4/5/both operation, pause/resume, SMART/FULL/UNIQUE, FULL sampling,
mode changes preserving session state, identical retransmissions and retry limit,
ACK timeout, STOP preserving pending data, reconnect validation, storage NACK,
115200/460800/921600 with ten PING round trips each, and deliberate restart with
pending records. The restart correctly loses the volatile queue, retains NVS,
and rejects the old boot identity.

Acceptance processed 243 distinct durable host records,
checked 2 eligible WiGLE rows, and detected no emitted CRC errors.
The expected ACK_TIMEOUT was injected by withholding ACK. Storage NACK models
an unavailable/full receiver, not a real microSD failure. The GPS policy-recovery
case was skipped when initial HDOP did not meet the configured threshold; HDOP
around 20 was observed during diagnostics and was correctly rejected against the
default maximum of 5. Validity thresholds were not relaxed to manufacture a pass.

Across the 12-cycle memory run, every drained sample had
182,892 B free internal RAM and
2,689,692 B free PSRAM. No scan error or queue overflow
occurred. This bounded test does not prove indefinite operation or high-density
physical RF performance.

## Reproduction

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
# Activate ESP-IDF 6.0.0 first:
idf.py build
python tools/stress_test.py PORT --output /tmp/acceptance.json
python tools/scan_benchmark.py PORT --cycles 6 --output /tmp/cadence.json
python tools/stress_test.py PORT --soak-cycles 12 --output /tmp/soak.json
```

These hardware tools temporarily change settings and initiate passive scans.
Acceptance intentionally resets the board. The bridge can also reset on port
open. Use no active FAP or other process on the UART during these tests.

## Evidence and remaining work

- [Build and host summary](../artifacts/validation/2026-11-23-continuous.json)
- [Physical acceptance](../artifacts/hardware/2026-11-23-continuous-acceptance.json)
- [Previous cadence baseline](../artifacts/hardware/2026-11-23-cadence-baseline.json)
- [Final cadence measurements](../artifacts/hardware/2026-11-23-continuous-cadence.json)
- [Final memory measurements](../artifacts/hardware/2026-11-23-continuous-soak.json)
- [FAP changes required](FLIPPER_INTEGRATION.md#continuous-driving-profile-in-040)

No FAP source was added. Actual microSD durability/full-card recovery, physical
cable disconnection, moving GNSS accuracy, repeatable good-fix loss/recovery,
long-duration dense RF operation, and WiGLE server import remain unverified.
Timestamp/GPS pairing is software-based and limited by receiver latency and
sampling frequency. One radio still visits channels sequentially. Neither the
8 MiB PSRAM nor the reception cache survives loss of power.
