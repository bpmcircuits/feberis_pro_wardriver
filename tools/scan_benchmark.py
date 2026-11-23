"""Compare passive scan cadence and receiver throughput; reports contain no RF identities or positions."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import time

from stress_test import Device


class TimedDevice(Device):
    def __init__(self, port, journal):
        super().__init__(port, journal)
        self.starts = []
        self.finishes = []

    def event(self, raw, fields):
        now = time.monotonic()
        if fields[2] == 'SCAN_BEGIN':
            self.starts.append(now)
        if fields[2] == 'SCAN_DONE':
            self.finishes.append(now)
        super().event(raw, fields)


def benchmark(d, result, cycles):
    d.pump(3)
    hello = d.command('HELLO', 2, expected=('OK', 'HELLO'))
    d.boot = int(hello[5])
    result['firmware'] = hello[4]
    state = d.state()
    assert not state['running'] and not state['queued']
    d.last_committed = state['committed']
    original = list(map(int, d.command('GET_CONFIG')[3:]))
    result['profiles'] = []
    try:
        for band in (1, 0):
            for gap in (1000, 0):
                c = original.copy()
                c[1], c[2], c[3], c[4], c[11] = 1, band, 0, 120, 0
                d.command('SET_CONFIG', *c, expected=('OK', 'SET_CONFIG'))
                d.starts.clear()
                d.finishes.clear()
                start = d.command('START_SCAN', gap)
                if start[2:4] == ['ERR', 'BAD_ARGUMENT']:
                    result['profiles'].append(dict(band=band, gap_ms=gap, supported=False))
                    continue
                assert start[2:4] == ['OK', 'START']
                d.wait(lambda: len(d.finishes) >= cycles, timeout=90)
                d.command('PAUSE_SCAN', expected=('OK', 'PAUSE_SCAN'))
                d.settle(running=1, paused=1)
                periods = [(b-a)*1000 for a, b in zip(d.starts, d.starts[1:])]
                profile = dict(band=band, gap_ms=gap, supported=True,
                               periods_ms=[round(v, 2) for v in periods],
                               median_period_ms=round(statistics.median(periods), 2),
                               stats=d.stats(), memory=d.memory())
                metrics = d.command('GET_SCAN_METRICS')
                if metrics[2] == 'SCAN_METRICS':
                    assert len(metrics) == 26
                    names = ('boot session requested_gap_ms dwell_ms capture_ready radio_ms ingest_ms '
                             'gap_ms max_gap_ms period_ms ingest_cpu_us captured dropped malformed '
                             'cache_overflows metadata_hits metadata_misses queue_high_water '
                             'backpressure_ms scan_buffer_bytes capture_cache_bytes main_stack_free '
                             'scan_capacity').split()
                    profile['metrics'] = dict(zip(names, map(int, metrics[3:])))
                    assert profile['metrics']['capture_ready']
                    assert profile['metrics']['metadata_hits'] > 0
                    assert profile['stats']['scan_errors'] == 0
                d.stop()
                result['profiles'].append(profile)
                print('BENCH', json.dumps({k: v for k, v in profile.items()
                                           if k in ('band', 'gap_ms', 'median_period_ms')}), flush=True)
    finally:
        d.stop()
        d.command('SET_CONFIG', *original, expected=('OK', 'SET_CONFIG'))
        result['original_config_restored'] = list(map(int, d.command('GET_CONFIG')[3:])) == original
        result['final_state'] = d.state()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--cycles', type=int, default=6)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 3 <= args.cycles <= 100:
        parser.error('cycles must be 3..100')
    result = dict(test='passive_scan_cadence', cycles=args.cycles)
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='feberis-benchmark-') as directory:
            with open(Path(directory)/'receiver.journal', 'w+b') as journal:
                d = TimedDevice(args.port, journal)
                try:
                    benchmark(d, result, args.cycles)
                    result['status'] = 'passed'
                finally:
                    d.close()
    except Exception as exc:
        result['status'] = 'failed'
        result['failure'] = f'{type(exc).__name__}: {exc}'
        raise
    finally:
        result['duration_seconds'] = round(time.monotonic()-started, 2)
        image = Path(__file__).resolve().parents[1]/'build'/'feberis_wardriver.bin'
        if image.exists():
            result['local_binary_sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
