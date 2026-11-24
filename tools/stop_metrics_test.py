"""Check STOP during physical scanning preserves the last ingestion metrics.

Uses passive scans and a temporary durable host receiver, not Flipper SD.
Restores the original configuration and leaves the device stopped and drained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile

from stress_test import Device


def metrics(device):
    fields = device.command('GET_SCAN_METRICS', expected=('SCAN_METRICS',))
    assert len(fields) == 26
    return {'radio_ms': int(fields[8]), 'ingest_cpu_us': int(fields[13])}


def run(device, result):
    device.pump(3)
    hello = device.command('HELLO', 2, expected=('OK', 'HELLO'))
    device.boot = int(hello[5])
    state = device.state()
    assert not state['running'] and not state['queued'], 'Existing session is active'
    device.last_committed = state['committed']
    original = list(map(int, device.command('GET_CONFIG')[3:]))
    result['cases'] = []
    try:
        config = original.copy()
        config[1:5] = [1, 1, 0, 120]  # FULL, 2.4 GHz, passive.
        config[11] = 0
        device.command('SET_CONFIG', *config, expected=('OK', 'SET_CONFIG'))
        for _ in range(3):
            starts = device.begins
            device.command('START_SCAN', 0, expected=('OK', 'START'))
            device.wait(lambda: device.begins >= starts + 2)
            before = metrics(device)
            assert before['ingest_cpu_us'] > 0, 'No completed nonempty ingestion batch'
            assert device.state()['scanning'], 'Missed in-flight scan window'
            device.stop()
            after = metrics(device)
            assert before == after, 'STOP overwrote completed ingestion metrics'
            result['cases'].append({'before': before, 'after': after})
            print('PASS STOP preserves ingestion metrics', after, flush=True)
        stats = device.stats()
        assert stats['scan_errors'] == stats['queue_overflows'] == 0
        result['stats'] = stats
        result['csv_rows_checked'] = device.csv_rows
        result['crc_errors'] = device.invalid_crc
    finally:
        device.stop()
        device.command('SET_CONFIG', *original, expected=('OK', 'SET_CONFIG'))
        result['original_config_restored'] = (
            list(map(int, device.command('GET_CONFIG')[3:])) == original)
        result['final_state'] = device.state()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = Path(__file__).resolve().parents[1] / 'build/feberis_wardriver.bin'
    result = {'test': 'physical_stop_metrics', 'flipper_sd_tested': False,
              'local_binary_sha256': hashlib.sha256(image.read_bytes()).hexdigest()}
    try:
        with tempfile.TemporaryFile('w+b') as journal:
            device = Device(args.port, journal)
            try:
                run(device, result)
            finally:
                device.close()
        result['status'] = 'passed'
    except Exception as exc:
        result.update(status='failed', failure=f'{type(exc).__name__}: {exc}')
        raise
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
