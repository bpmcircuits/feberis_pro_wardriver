"""Exercise real FBR1 hardware over USB-UART (requires pyserial)."""
import argparse
import json
import time
from pathlib import Path

import serial
from protocol import decode, encode


class Device:
    def __init__(self, port):
        self.link = serial.Serial()
        self.link.port = port
        self.link.baudrate = 115200
        self.link.timeout = 0.1
        self.link.dtr = False
        self.link.rts = False
        self.link.open()
        # Explicitly release both lines, then pulse EN; the bridge may retain
        # control-line levels from its previous connection.
        self.link.dtr = False
        self.link.rts = True
        time.sleep(0.1)
        self.link.rts = False
        self.request_id = 0
        self.frames = []
        self.buffer = bytearray()

    def send(self, command, *args):
        self.request_id += 1
        self.link.write(encode(['FBR1', str(self.request_id), command, *map(str, args)]))
        return str(self.request_id)

    def receive(self, deadline):
        while time.monotonic() < deadline:
            byte = self.link.read(1)
            if not byte:
                continue
            self.buffer.extend(byte)
            if byte == b'\n':
                raw = bytes(self.buffer)
                self.buffer.clear()
                if not raw.startswith(b'FBR1\t'):
                    continue
                fields = decode(raw)
                self.frames.append(fields)
                if fields[2] not in ('AP', 'STATUS'):
                    print('RX', fields, flush=True)
                return raw, fields
            if len(self.buffer) > 4096:
                self.buffer.clear()
        raise TimeoutError('No expected frame before deadline')

    def until(self, predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while True:
            raw, fields = self.receive(deadline)
            if predicate(fields):
                return raw, fields

    def command(self, command, *args, expected=('OK',)):
        request_id = self.send(command, *args)
        _, fields = self.until(lambda f: f[1] == request_id)
        assert tuple(fields[2:2 + len(expected)]) == expected, fields
        return fields

    def idle(self):
        self.until(lambda f: f[2] == 'STATUS' and f[4:6] == ['0', 'IDLE'])


def run(device, result):
    device.until(lambda f: f[2] == 'STATUS', seconds=15)
    result['hello'] = device.command('HELLO', expected=('OK', 'HELLO'))
    device.command('STOP', expected=('OK', 'STOP'))
    device.idle()
    device.command('PING', expected=('OK', 'PONG'))
    status = device.command('STATUS', expected=('STATUS',))
    assert len(status) == 24 and status[22:24] == ['1', '1'], status
    result['initial_status'] = status
    device.command('START', 999, expected=('ERR', 'BAD_ARGUMENT'))
    # A corrupt CRC must not execute START.
    bad = encode(['FBR1', '4000000000', 'START', '1000'])
    device.link.write(bad[:-5] + b'0000\n')
    status = device.command('STATUS', expected=('STATUS',))
    assert status[4] == '0', status
    result['scans'] = {}
    for band in ('2G', '5G', 'AUTO'):
        device.command('BAND', band, expected=('OK', 'BAND'))
        device.command('START', 60000, expected=('OK', 'START'))
        device.until(lambda f: f[2] == 'SCAN_BEGIN', seconds=2)
        device.command('BAND', 'AUTO', expected=('ERR', 'BUSY'))
        deadline = time.monotonic() + 45
        count = 0
        channels = set()
        retry_checked = False
        while True:
            raw, f = device.receive(deadline)
            assert f[2] != 'ERROR', f
            if f[2] == 'AP':
                assert len(f) == 20, f
                channel = int(f[9])
                assert band != '2G' or channel <= 14, channel
                assert band != '5G' or channel > 14, channel
                channels.add(channel)
                if f[11] == '0':
                    assert f[19] == '-', f
                if not retry_checked:
                    repeated, _ = device.until(lambda x: x[2] == 'AP', seconds=4)
                    assert repeated == raw, 'Retransmission changed bytes'
                    retry_checked = True
                device.send('ACK', f[3], f[4])
                count += 1
            elif f[2] == 'SCAN_DONE':
                assert int(f[6]) == count, (f, count)
                result['scans'][band] = dict(records=count, channels=sorted(channels),
                                             retransmission_checked=retry_checked)
                print(band, result['scans'][band], flush=True)
                break
        device.command('STOP', expected=('OK', 'STOP'))
        device.idle()
    # Omit every ACK: expect four identical transmissions, then a stopped session.
    device.command('BAND', '2G')
    device.command('START', 60000)
    first, _ = device.until(lambda f: f[2] == 'AP', seconds=35)
    for _ in range(3):
        repeated, _ = device.until(lambda f: f[2] == 'AP', seconds=4)
        assert repeated == first
    device.until(lambda f: f[2:4] == ['ERROR', 'ACK_TIMEOUT'], seconds=4)
    device.until(lambda f: f[2] == 'STOPPED' and f[4] == 'ACK_TIMEOUT')
    device.idle()
    result['ack_timeout'] = 'passed'
    device.command('BAND', 'AUTO')
    result['final_status'] = device.command('STATUS', expected=('STATUS',))
    result['passed'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    result = {'passed': False}
    device = Device(args.port)
    try:
        run(device, result)
    except Exception as error:
        result['error'] = str(error)
        result['recent_frames'] = [f if f[2] != 'AP' else f[:6]
                                   for f in device.frames[-20:]]
        raise
    finally:
        try:
            device.command('STOP')
        finally:
            device.link.close()
            Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
