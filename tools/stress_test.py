"""Passive hardware acceptance tests. Temporary host journal models a durable receiver, not Flipper SD."""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import tempfile
import time

import serial
from protocol import MAX_FRAME, decode, encode

STATS = ('total_detections unique_aps observations_saved observations_filtered '
         'gps_valid_observations gps_invalid_observations wifi_2g_aps wifi_5g_aps '
         'open_aps secured_aps scan_cycles scan_errors uart_retransmissions '
         'uart_dropped_frames storage_errors queue_overflows table_overflows '
         'queued committed free_heap minimum_free_heap').split()


class Device:
    def __init__(self, port, journal):
        self.link = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=3)
        self.link.dtr = self.link.rts = False
        self.link.port = port
        self.link.open()
        self.journal = journal
        self.request_id = 100
        self.buffer = bytearray()
        self.dropping = False
        self.boot = None
        self.session = 0
        self.last_committed = 0
        self.auto_ack = True
        self.commit_enabled = True
        self.hashes = {}
        self.persisted = set()
        self.bands = set()
        self.cycles = self.begins = self.duplicates = self.csv_rows = 0
        self.invalid_crc = 0
        self.errors = []
        self.observations = 0
        self.gps_recovered = 0

    def send(self, name, *args):
        self.request_id += 1
        self.link.write(encode(['FBR1', str(self.request_id), name, *map(str, args)]))
        return str(self.request_id)

    def receive(self, timeout=0.2):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            byte = self.link.read(1)
            if not byte:
                continue
            if byte != b'\n':
                if not self.dropping:
                    self.buffer.extend(byte)
                    if len(self.buffer) >= MAX_FRAME:
                        self.buffer.clear()
                        self.dropping = True
                continue
            raw = bytes(self.buffer) + byte
            self.buffer.clear()
            if self.dropping:
                self.dropping = False
                continue
            try:
                f = decode(raw)
            except (ValueError, UnicodeError):
                if raw.startswith(b'FBR1\t'):
                    self.invalid_crc += 1
                    raise AssertionError('Firmware emitted a damaged FBR1 frame')
                continue
            self.event(raw, f)
            return f
        return None

    def event(self, raw, f):
        kind = f[2]
        if kind == 'READY' and self.boot is not None:
            assert int(f[4]) == self.boot, 'Unexpected device restart'
        if kind in ('STATUS', 'SCAN_BEGIN', 'SCAN_DONE', 'OBSERVATION', 'SESSION_STARTED'):
            if self.boot is not None:
                assert int(f[3]) == self.boot, 'Boot changed unexpectedly'
        if kind == 'SESSION_STARTED':
            self.session = int(f[4])
        if kind == 'SCAN_BEGIN':
            self.begins += 1
        if kind == 'SCAN_DONE':
            self.cycles += 1
        if kind == 'ERROR':
            self.errors.append(f[3])
        if kind != 'OBSERVATION':
            return
        assert len(f) == 28, 'Wrong observation schema'
        key = tuple(map(int, (f[3], f[5], f[4])))
        digest = hashlib.sha256(raw).digest()
        if key in self.hashes:
            assert self.hashes[key] == digest, 'Retransmission changed its bytes'
            self.duplicates += 1
        else:
            self.hashes[key] = digest
            self.observations += 1
            if int(f[22]) & 32:
                self.gps_recovered += 1
        ssid = bytes.fromhex(f[7])
        assert len(ssid) <= 32
        channel, rssi, valid = map(int, (f[9], f[10], f[11]))
        band = int(f[20])
        assert band in (2, 5) and (channel <= 14) == (band == 2)
        assert -127 <= rssi <= 0 and valid in (0, 1)
        self.bands.add(band)
        if valid:
            assert math.isfinite(float(f[13])) and abs(float(f[13])) <= 90
            assert math.isfinite(float(f[14])) and abs(float(f[14])) <= 180
        else:
            assert math.isnan(float(f[13])) and math.isnan(float(f[14]))
            assert f[19] == '-', 'CSV exported without a valid GPS fix'
        if f[19] != '-':
            rows = list(csv.reader(io.StringIO(bytes.fromhex(f[19]).decode('ascii'))))
            assert valid and len(rows) == 1 and len(rows[0]) == 11
            row = rows[0]
            assert row[0] == f[6] and row[2] == f[8] and row[3] == f[12]
            assert int(row[4]) == channel and int(row[5]) == rssi
            assert abs(float(row[6]) - float(f[13])) < 1e-7
            assert abs(float(row[7]) - float(f[14])) < 1e-7
            assert row[10] == 'WIFI'
            assert row[9] == '' or float(row[9]) > 0
        if self.commit_enabled and key not in self.persisted:
            self.journal.write(raw)
            self.journal.flush()
            os.fsync(self.journal.fileno())
            self.persisted.add(key)
            self.last_committed = key[2]
            if f[19] != '-':
                self.csv_rows += 1
        if self.auto_ack and key in self.persisted:
            self.send('ACK', *key)

    def command(self, name, *args, expected=None, timeout=8):
        request = self.send(name, *args)
        next_send = time.monotonic() + 1
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if name in ('GET_INFO', 'GET_MEMORY', 'GET_STATUS', 'GET_CONFIG', 'GET_STATS', 'GET_AP', 'GET_GPS', 'STATUS', 'PING') and time.monotonic() >= next_send:
                self.link.write(encode(['FBR1', request, name, *map(str,args)]))
                next_send = time.monotonic() + 1
            f = self.receive(0.2)
            if f and f[1] == request:
                if expected is not None:
                    assert f[2:2 + len(expected)] == list(expected), f'{name}: unexpected response type {f[2:4]}'
                return f
        raise TimeoutError(f'{name}: response timeout')

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.receive(min(0.2, deadline - time.monotonic()))

    def wait(self, predicate, timeout=35):
        deadline = time.monotonic() + timeout
        while not predicate():
            assert time.monotonic() < deadline, 'Timed out waiting for device state'
            self.receive(0.2)

    def state(self):
        f = self.command('GET_STATUS', expected=('STATE',))
        return dict(zip(('boot session running paused scanning link_wait queued last_sequence committed elapsed_ms').split(), map(int, f[3:])))

    def stats(self):
        f = self.command('GET_STATS', expected=('STATS',))
        assert len(f) == 26
        return dict(zip(STATS, map(int, f[5:])))

    def memory(self):
        f = self.command('GET_MEMORY', expected=('MEMORY',))
        assert len(f) == 18 and f[3:5] == ['1', '1']
        return list(map(int, f[3:]))

    def settle(self, running=0, paused=None, timeout=35):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.state()
            old = self.command('STATUS', expected=('STATUS',))
            if (state['running'] == running and not state['scanning'] and not state['queued']
                    and old[5] == 'IDLE' and (paused is None or state['paused'] == paused)):
                return state
            self.pump(0.2)
        raise TimeoutError('Could not drain device')

    def stop(self):
        self.auto_ack = self.commit_enabled = True
        self.command('HELLO', 2, expected=('OK', 'HELLO'))
        self.command('STOP_SCAN', expected=('OK', 'STOP'))
        return self.settle()

    def close(self):
        self.link.close()


def run(d, result):
    # Allow boot/memory test to complete before the first command.
    d.pump(3)
    hello = d.command('HELLO', 2, expected=('OK', 'HELLO'))
    d.boot = int(hello[5])
    result['firmware'] = hello[4]
    initial = d.state()
    assert not initial['running'] and not initial['queued'], 'An existing session is active; test aborted'
    d.last_committed = initial['committed']
    original = list(map(int, d.command('GET_CONFIG', expected=('CONFIG',))[3:]))
    result['initial_memory'] = d.memory()
    result['cases'] = []

    def passed(name, **data):
        result['cases'].append(dict(name=name, status='passed', **data))
        print('PASS', name, json.dumps(data), flush=True)

    def configure(mode=0, band=0, **kwargs):
        names = ('version mode band active dwell_ms min_distance_m rssi_delta_db max_gap_ms '
                 'min_gap_ms gps_max_age_ms gps_max_hdop_x10 full_gap_ms baud').split()
        c = dict(zip(names, original))
        c.update(mode=mode, band=band, active=0, **kwargs)
        values = [c[k] for k in names]
        d.command('SET_CONFIG', *values, expected=('OK', 'SET_CONFIG'))
        assert list(map(int, d.command('GET_CONFIG')[3:])) == values
        return values

    try:
        d.command('PING', expected=('OK', 'PONG'))
        d.command('START_SCAN', 999, expected=('ERR', 'BAD_ARGUMENT'))
        d.command('START_SCAN', 4294967296, expected=('ERR', 'BAD_ARGUMENT'))
        bad = bytearray(encode(['FBR1', '50', 'START_SCAN', '1000']))
        bad[-2] = ord('0') if bad[-2] != ord('0') else ord('1')
        d.link.write(bad)
        d.link.write(b'X' * 2000 + b'\n')
        d.command('PING', expected=('OK', 'PONG'))
        assert not d.state()['running']
        invalid = original.copy()
        invalid[1] = 3
        d.command('SET_CONFIG', *invalid, expected=('ERR', 'BAD_ARGUMENT'))
        assert list(map(int, d.command('GET_CONFIG')[3:])) == original
        passed('invalid_crc_overlong_frame_and_arguments')

        for band in (1, 2, 0):
            configure(mode=1, band=band, full_gap_ms=0)
            before = d.cycles
            old_obs = d.observations
            d.bands.clear()
            d.command('START_SCAN', 1000, expected=('OK', 'START'))
            d.command('RESET_STATS', expected=('ERR', 'BUSY'))
            d.command('BAND', 'AUTO', expected=('ERR', 'BUSY'))
            d.wait(lambda: d.cycles > before)
            d.command('PAUSE_SCAN', expected=('OK', 'PAUSE_SCAN'))
            d.settle(running=1, paused=1)
            s = d.stats()
            assert s['observations_saved'] == s['total_detections'] and not s['observations_filtered']
            assert s['gps_valid_observations'] + s['gps_invalid_observations'] == s['observations_saved']
            assert not s['scan_errors'] and not s['queue_overflows']
            if band:
                assert d.bands <= {2 if band == 1 else 5}
            cycles = d.cycles
            d.pump(2.2)
            assert d.cycles == cycles, 'PAUSE allowed another scan'
            d.command('RESUME_SCAN', expected=('OK', 'RESUME_SCAN'))
            d.wait(lambda: d.cycles > cycles)
            d.stop()
            passed(f'passive_full_band_{band}_pause_resume', detections=d.stats()['total_detections'],
                   observations=d.observations-old_obs, bands=sorted(d.bands))

        for mode in (2, 0):
            configure(mode=mode, band=1)
            before = d.cycles
            d.command('START_SCAN', 1000, expected=('OK', 'START'))
            d.wait(lambda: d.cycles >= before + 3)
            d.command('PAUSE_SCAN', expected=('OK', 'PAUSE_SCAN'))
            d.settle(running=1, paused=1)
            s = d.stats()
            assert s['total_detections'] == s['observations_saved'] + s['observations_filtered']
            if mode == 2:
                assert s['observations_saved'] == s['unique_aps']
            summed = detections = 0
            for slot in range(s['unique_aps']):
                f = d.command('GET_AP', slot)
                assert len(f) == 29 and f[2] in ('AP_DISCOVERED', 'AP_UPDATED')
                detections += int(f[14])
                summed += int(f[15])
                if mode == 2:
                    assert int(f[15]) == 1
            assert summed == s['observations_saved'] and detections == s['total_detections']
            d.stop()
            passed('unique' if mode == 2 else 'smart', unique=s['unique_aps'],
                   detections=s['total_detections'], observations=s['observations_saved'], filtered=s['observations_filtered'])

        configure(mode=1, band=1, full_gap_ms=60000)
        cycle = d.cycles
        d.command('START_SCAN',1000,expected=('OK','START'))
        d.wait(lambda: d.cycles>=cycle+2)
        d.command('PAUSE_SCAN',expected=('OK','PAUSE_SCAN'))
        d.settle(running=1,paused=1)
        sampled=d.stats()
        assert sampled['observations_saved']==sampled['unique_aps']
        d.stop()
        passed('full_sampling_limit',detections=sampled['total_detections'],observations=sampled['observations_saved'])

        # Switch modes within one session without resetting AP baselines or counts.
        configure(mode=1,band=1,full_gap_ms=0)
        cycle=d.cycles
        d.command('START_SCAN',1000,expected=('OK','START'))
        d.wait(lambda:d.cycles>cycle)
        d.command('PAUSE_SCAN',expected=('OK','PAUSE_SCAN'))
        state=d.settle(running=1,paused=1)
        before=d.stats()
        configure(mode=2,band=1)
        d.command('RESUME_SCAN',expected=('OK','RESUME_SCAN'))
        cycle=d.cycles
        d.wait(lambda:d.cycles>cycle)
        d.command('PAUSE_SCAN',expected=('OK','PAUSE_SCAN'))
        d.settle(running=1,paused=1)
        after=d.stats()
        assert after['observations_saved']-before['observations_saved']==after['unique_aps']-before['unique_aps']
        assert d.state()['session']==state['session']
        configure(mode=0,band=1,min_gap_ms=1000,max_gap_ms=1000)
        d.command('RESUME_SCAN',expected=('OK','RESUME_SCAN'))
        cycle=d.cycles
        d.wait(lambda:d.cycles>cycle)
        d.stop()
        assert d.stats()['total_detections']>=after['total_detections']
        passed('mode_changes_preserve_session_and_baselines')

        gps=d.command('GET_GPS',expected=('GPS',))
        if gps[3]=='1':
            configure(mode=0,band=1,gps_max_hdop_x10=1,gps_max_age_ms=100)
            cycle=d.cycles
            csv_before=d.csv_rows
            d.command('START_SCAN',1000,expected=('OK','START'))
            d.wait(lambda:d.cycles>cycle)
            d.command('PAUSE_SCAN',expected=('OK','PAUSE_SCAN'))
            d.settle(running=1,paused=1)
            no_fix=d.stats()
            assert no_fix['gps_invalid_observations']==no_fix['observations_saved']
            assert d.csv_rows==csv_before
            recovered=d.gps_recovered
            configure(mode=0,band=1,gps_max_hdop_x10=50,gps_max_age_ms=3000)
            cycle=d.cycles
            d.command('RESUME_SCAN',expected=('OK','RESUME_SCAN'))
            d.wait(lambda:d.cycles>cycle)
            d.stop()
            assert d.stats()['gps_valid_observations']>0 and d.gps_recovered>recovered
            passed('gps_policy_invalid_then_recovered_without_stopping_wifi')
        else:
            result['cases'].append(dict(name='gps_policy_invalid_then_recovered_without_stopping_wifi',status='not_run',reason='No valid initial fix'))

        configure(mode=1, band=1, full_gap_ms=0)
        d.auto_ack = False
        obs = d.observations
        duplicate = d.duplicates
        d.command('START_SCAN', 1000, expected=('OK', 'START'))
        d.wait(lambda: d.observations > obs)
        state = d.state()
        session = state['session']
        d.command('PAUSE_SCAN', expected=('OK', 'PAUSE_SCAN'))
        d.wait(lambda: d.duplicates >= duplicate+3, timeout=12)
        d.pump(2.3)
        stalled = d.state()
        assert stalled['link_wait'] and stalled['queued'] >= 1
        d.command('START_SCAN', 1000, expected=('OK', 'START'))  # same session, no reset
        assert d.state()['session'] == session
        d.command('STOP_SCAN', expected=('OK', 'STOP'))
        assert d.state()['queued'] >= stalled['queued']
        d.command('START_SCAN', 1000, expected=('ERR', 'BUSY'))
        d.command('RESUME_LINK', d.boot ^ 1, session, d.last_committed, expected=('ERR', 'SESSION_MISMATCH'))
        d.command('RESUME_LINK', d.boot, session, 4294967295, expected=('ERR', 'SEQUENCE_MISMATCH'))
        # The first record is durable on the host, its ACK was deliberately lost.
        d.command('RESUME_LINK', d.boot, session, d.last_committed, expected=('OK', 'RESUME_LINK'))
        d.auto_ack = True
        d.settle()
        passed('identical_retries_timeout_stop_preserves_queue_and_resume', retries=d.stats()['uart_retransmissions'])

        configure(mode=1, band=1, full_gap_ms=0)
        d.auto_ack = d.commit_enabled = False
        obs = d.observations
        d.command('START_SCAN', 1000, expected=('OK', 'START'))
        d.wait(lambda: d.observations > obs)
        d.command('NACK', 'STORAGE', expected=('OK', 'NACK'))
        state = d.state()
        assert state['paused'] and state['link_wait'] and state['queued'] >= 1
        d.pump(2.3)
        assert d.state()['queued'] >= state['queued']
        assert d.stats()['storage_errors'] == 1
        d.auto_ack = d.commit_enabled = True
        d.command('RESUME_LINK', d.boot, state['session'], d.last_committed, expected=('OK', 'RESUME_LINK'))
        d.command('RESUME_SCAN', expected=('OK', 'RESUME_SCAN'))
        d.stop()
        passed('storage_nack_preserves_uncommitted_records')

        # UART speeds are changed only with no session or pending record.
        for baud in (460800, 921600, original[-1]):
            c = list(map(int, d.command('GET_CONFIG')[3:]))
            c[-1] = baud
            d.command('SET_CONFIG', *c, expected=('OK', 'SET_CONFIG'))
            time.sleep(0.25)
            d.link.baudrate = baud
            info = d.command('GET_INFO', expected=('INFO',))
            assert int(info[5]) == baud
            for _ in range(10):
                d.command('PING', expected=('OK', 'PONG'))
            cycle = d.cycles
            d.command('START_SCAN',1000,expected=('OK','START'))
            d.wait(lambda: d.cycles>cycle)
            d.stop()
            passed(f'uart_{baud}', ping_roundtrips=10)

        result['memory_before_restart'] = d.memory()
        result['boot_unchanged_before_intentional_restart'] = d.state()['boot']==d.boot
        configure(mode=1,band=1,full_gap_ms=0)
        restart_config=list(map(int,d.command('GET_CONFIG')[3:]))
        d.auto_ack=False
        observations=d.observations
        d.command('START_SCAN',1000,expected=('OK','START'))
        d.wait(lambda:d.observations>observations)
        old=d.state()
        assert old['queued']>0
        old_boot=d.boot
        d.boot=None
        d.link.rts=True
        time.sleep(0.1)
        d.link.rts=False
        d.buffer.clear()
        d.dropping=False
        d.auto_ack=True
        d.pump(3)
        hello=d.command('HELLO',2,expected=('OK','HELLO'))
        d.boot=int(hello[5])
        assert d.boot!=old_boot
        d.last_committed=0
        assert list(map(int,d.command('GET_CONFIG')[3:]))==restart_config
        reset=d.state()
        assert reset['session']==0 and reset['queued']==0 and reset['running']==0
        assert d.memory()[12]==0
        d.command('RESUME_LINK',old_boot,old['session'],old['committed'],expected=('ERR','SESSION_MISMATCH'))
        d.command('ACK',old_boot,old['session'],old['committed'],expected=('ERR','BOOT_MISMATCH'))
        passed('restart_with_pending_records_nvs_persists_and_stale_boot_rejected',queued_before_restart=old['queued'],queued_after_restart=0)

        result['final_memory'] = d.memory()
        assert result['final_memory'][9] == result['initial_memory'][9]
        assert result['final_memory'][11:14:2] == result['initial_memory'][11:14:2]
        result['crc_errors'] = d.invalid_crc
        result['unique_records_in_host_journal'] = len(d.persisted)
        result['csv_rows_checked'] = d.csv_rows
        result['duplicate_transmissions'] = d.duplicates
        result['errors_observed'] = sorted(set(d.errors))
    finally:
        d.auto_ack = d.commit_enabled = True
        d.stop()
        d.command('SET_CONFIG', *original, expected=('OK', 'SET_CONFIG'))
        time.sleep(0.25)
        d.link.baudrate = original[-1]
        result['original_config_restored'] = list(map(int, d.command('GET_CONFIG')[3:])) == original
        result['final_state'] = d.state()



def run_soak(d, result, cycles):
    d.pump(3)
    hello=d.command('HELLO',2,expected=('OK','HELLO'))
    d.boot=int(hello[5])
    result['firmware']=hello[4]
    state=d.state()
    assert not state['running'] and not state['queued']
    d.last_committed=state['committed']
    original=list(map(int,d.command('GET_CONFIG')[3:]))
    c=original.copy()
    c[1]=1; c[2]=1; c[3]=0; c[4]=120; c[11]=0
    samples=[]
    try:
        d.command('SET_CONFIG',*c,expected=('OK','SET_CONFIG'))
        d.command('START_SCAN',1000,expected=('OK','START'))
        for i in range(cycles):
            before=d.cycles
            if i:
                d.command('RESUME_SCAN',expected=('OK','RESUME_SCAN'))
            d.wait(lambda:d.cycles>before)
            d.command('PAUSE_SCAN',expected=('OK','PAUSE_SCAN'))
            d.settle(running=1,paused=1)
            memory=d.memory()
            samples.append(dict(cycle=i+1,internal_free=memory[6],internal_minimum=memory[7],
                psram_free=memory[3],storage_reserved=memory[9]))
            print('SOAK',json.dumps(samples[-1]),flush=True)
        assert all(s['storage_reserved']==5304064 for s in samples)
        assert len({s['psram_free'] for s in samples})==1
        assert samples[-1]['internal_free']>=samples[0]['internal_free']-4096
        stats=d.stats()
        assert not stats['scan_errors'] and not stats['queue_overflows']
        result.update(samples=samples,stats=stats,crc_errors=d.invalid_crc,
            unique_records_in_host_journal=len(d.persisted),csv_rows_checked=d.csv_rows)
    finally:
        d.stop()
        d.command('SET_CONFIG',*original,expected=('OK','SET_CONFIG'))
        result['original_config_restored']=list(map(int,d.command('GET_CONFIG')[3:]))==original
        result['final_state']=d.state()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--soak-cycles',type=int,default=0,help='Run only a bounded heap stability test (1..100 cycles)')
    args = parser.parse_args()
    if not 0<=args.soak_cycles<=100:
        parser.error('soak-cycles must be 0..100')
    started = time.monotonic()
    result = {'test': 'passive_hardware_acceptance', 'flipper_sd_tested': False,
              'physical_cable_disconnect_tested': False}
    d = None
    try:
        # Journal has a real temporary filename and is fsynced before ACK.
        # It contains test RF/GPS records only and is removed after the run.
        with tempfile.TemporaryDirectory(prefix='feberis-acceptance-') as directory:
            with open(Path(directory)/'receiver.journal', 'w+b') as journal:
                d = Device(args.port, journal)
                try:
                    if args.soak_cycles:
                        result['test']='passive_heap_soak'
                        run_soak(d,result,args.soak_cycles)
                    else:
                        run(d, result)
                    result['status'] = 'passed'
                finally:
                    d.close()
    except Exception as exc:
        result['status'] = 'failed'
        result['failure'] = f'{type(exc).__name__}: {exc}'
        raise
    finally:
        result['duration_seconds'] = round(time.monotonic() - started, 2)
        local_image = Path(__file__).resolve().parents[1]/'build'/'feberis_wardriver.bin'
        if local_image.exists():
            result['local_binary_sha256'] = hashlib.sha256(local_image.read_bytes()).hexdigest()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n')
        print('RESULT', result['status'], str(args.output), flush=True)


if __name__ == '__main__':
    main()
