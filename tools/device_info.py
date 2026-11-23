"""Read firmware and memory diagnostics over USB-UART without pulsing reset or scanning."""
import argparse
import json
import time

import serial
from protocol import MAX_FRAME, decode, encode

MEMORY_FIELDS = (
    'storage_ready', 'psram_initialized', 'psram_total_bytes', 'psram_free_bytes',
    'psram_minimum_free_bytes', 'psram_largest_free_block', 'internal_free_bytes',
    'internal_minimum_free_bytes', 'internal_largest_free_block',
    'storage_reserved_bytes', 'ap_record_bytes', 'ap_capacity', 'ap_count',
    'queue_capacity', 'queued',
)


def inspect(port, baud):
    link = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=2)
    link.dtr = False
    link.rts = False
    link.port = port
    link.open()
    buffer = bytearray()
    dropping = False

    def command(request_id, name):
        nonlocal dropping
        frame = encode(['FBR1', str(request_id), name])
        link.write(frame)
        deadline = time.monotonic() + 12
        next_send = time.monotonic() + 1
        while time.monotonic() < deadline:
            # Some USB-UART bridges reset the target while opening the port.
            # Read-only requests are idempotent; repeat until application UART is ready.
            if time.monotonic() >= next_send:
                link.write(frame)
                next_send = time.monotonic() + 1
            byte = link.read(1)
            if not byte:
                continue
            if byte == b'\n':
                raw = bytes(buffer) + byte
                buffer.clear()
                if dropping:
                    dropping = False
                    continue
                try:
                    fields = decode(raw)
                except (ValueError, UnicodeError):
                    continue
                if len(fields) >= 3 and fields[1] == str(request_id):
                    return fields
            elif not dropping:
                buffer.extend(byte)
                if len(buffer) >= MAX_FRAME:
                    buffer.clear()
                    dropping = True
        raise TimeoutError(f'{name}: no response at {baud} baud')

    try:
        info = command(1001, 'GET_INFO')
        if len(info) != 10 or info[2] != 'INFO':
            raise ValueError(f'Unexpected GET_INFO response: {info}')
        memory = command(1002, 'GET_MEMORY')
        if len(memory) != 18 or memory[2] != 'MEMORY':
            raise ValueError(f'Unexpected GET_MEMORY response: {memory}')
        status = command(1003, 'STATUS')
        if len(status) != 24 or status[2] != 'STATUS':
            raise ValueError('Invalid STATUS response')
        ping = command(1004, 'PING')
        if ping[2:] != ['OK', 'PONG']:
            raise ValueError('PING failed')
        return {
            'firmware': info[3], 'boot': int(info[4]), 'baud': int(info[5]),
            'country': info[6], 'protocol_revision': int(info[9]),
            'memory': dict(zip(MEMORY_FIELDS, map(int, memory[3:]))),
            'running': bool(int(status[4])), 'gps_driver_ready': bool(int(status[22])),
            'wifi_driver_ready': bool(int(status[23])), 'ping': 'passed',
        }
    finally:
        link.close()


def capture_boot(port, baud):
    link = serial.Serial(port=None, baudrate=baud, timeout=0.1)
    link.dtr = False
    link.rts = False
    link.port = port
    link.open()
    try:
        link.rts = True
        time.sleep(0.1)
        link.rts = False
        data = bytearray()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and len(data) < 32768:
            data.extend(link.read(1024))
        return data.decode('utf-8', errors='replace')
    finally:
        link.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--baud', type=int, default=115200, choices=(115200, 460800, 921600))
    parser.add_argument('--boot-log', action='store_true', help='Explicitly pulse reset and capture boot output')
    args = parser.parse_args()
    if args.boot_log:
        print(capture_boot(args.port, args.baud))
        return
    print(json.dumps(inspect(args.port, args.baud), indent=2))


if __name__ == '__main__':
    main()
