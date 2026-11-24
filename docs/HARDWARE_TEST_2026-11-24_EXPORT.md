# Export corrections — validation on 2026-11-24

The corrected ESP32-C5 image was flashed through the Flipper USB–UART bridge
at 460800 baud. All written image hashes were verified. NVS was not erased.

Firmware image SHA-256:

```text
66c16aab53ab2e33716c0e2533713d68f3bcc24121d7d39260efd51a917076b5
```

## Physical firmware checks

- STOP during the second, in-flight passive 2.4 GHz scan preserved the previous
  successful batch's radio and ingestion timing in three separate sessions.
  Before/after ingestion times were 2025, 2271, and 2396 microseconds respectively.
- Acceptance passed 15 scenarios: invalid frames/arguments, scan gaps, all three
  passive band profiles, UNIQUE/SMART/FULL sampling, mode changes, identical
  retries and ACK timeout recovery, storage NACK, all three UART speeds, and an
  intentional restart with pending observations and persistent NVS settings.
- The temporary host receiver durably journaled 210 distinct observations and
  checked 11 retransmissions. No emitted CRC errors were detected.
- No qualifying initial GPS fix was available. GPS loss/recovery was skipped,
  and **zero WiGLE data rows were emitted or checked in the physical run**.
- Both tools restored the original settings and left scanning stopped with an
  empty observation queue. The expected ACK_TIMEOUT was injected deliberately.

Evidence: [STOP metrics](../artifacts/hardware/2026-11-24-stop-metrics.json) and
[acceptance](../artifacts/hardware/2026-11-24-export-acceptance.json).

The receiver ran on the computer; these checks do not validate the FAP or SD.
Authentication modes, UTF-8 names, and all-NUL SSIDs require suitable APs and a
valid GPS fix for physical WiGLE export coverage.

## Companion application

Inspection found that the separate companion application regenerates CSV from
validated journal fields instead of trusting firmware csv_hex. Its own exporter
therefore also required the UTF-8 and all-NUL corrections. Those changes and
regression tests were applied in the separate companion repository, not here.
GUI display escaping remains independent from CSV text.

The companion host protocol, session/storage, UI, and independent CSV/JSON
checks passed with ASan/UBSan. uFBT built the corrected FAP for target 7,
API 87.1. The final build is in the companion repository's
`dist/feberis_pro.fap`, with SHA-256:

```text
1e8822ad5ab4ed82702c43f23c9ccc8cd0fa5ca5fbe61d2ad31881896ff8b2f7
```

Physical FAP installation, SD session export, and WiGLE server import are
pending; do not infer those results from the host tests or build. After leaving
the USB–UART bridge, the Flipper identified itself as Momentum mntm-012 and
emitted its CLI welcome/prompt, but did not respond to `help` or `device_info`.
The official SDK storage tool also waited for a device_info response. It was
interrupted without writing to SD. A Flipper restart was requested to retry.

## Session handoff

The user stopped further USB/FAP testing and will install the companion FAP
manually, then perform a field drive. The corrected ESP firmware is already
installed and verified. The companion FAP has been built but was not installed
by this session. No further device actions are pending or authorized to resume
automatically.

When field results arrive, compare `wigle.csv`, `observations.csv`,
`access_points.csv`, `metadata.json`, and `stats.json` from the same session.
Check preserved UTF-8, empty all-NUL hidden SSIDs, security labels, exclusion of
UNKNOWN/RX_UNVERIFIED/no-fix observations, journal/export counts, scan metrics,
and GPS timing/validity. The earlier 23 November exports are historical evidence;
their UNKNOWN authentication modes cannot be reconstructed from those labels.
