"""Reference FBR1 framing. No third-party packages required."""
import argparse
import binascii
import sys

MAX_FRAME = 1536


def encode(fields):
    """Encode fields without CR; all arbitrary binary data must use hex fields."""
    if any(any(ord(c) < 32 or ord(c) > 126 or c == '*' for c in field) for field in fields):
        raise ValueError("fields must contain printable ASCII without '*'")
    body = '\t'.join(fields).encode('ascii')
    frame = body + f'*{binascii.crc_hqx(body, 0xffff):04X}\n'.encode('ascii')
    if len(frame) >= MAX_FRAME:
        raise ValueError('frame too large')
    return frame


def decode(frame):
    if len(frame) >= MAX_FRAME or not frame.endswith(b'\n') or b'\r' in frame:
        raise ValueError('invalid framing')
    body, checksum = frame[:-1].rsplit(b'*', 1)
    if len(checksum) != 4 or any(c not in b'0123456789abcdefABCDEF' for c in checksum):
        raise ValueError('invalid CRC encoding')
    if binascii.crc_hqx(body, 0xffff) != int(checksum, 16):
        raise ValueError('CRC mismatch')
    fields = body.decode('ascii').split('\t')
    if fields[0] != 'FBR1' or len(fields) > 48:
        raise ValueError('invalid protocol')
    if any(any(ord(c) < 32 or ord(c) > 126 or c == '*' for c in field) for field in fields):
        raise ValueError('invalid field')
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('request_id', type=int)
    parser.add_argument('command', choices=['HELLO', 'PING', 'STATUS', 'START', 'STOP', 'ACK', 'BAND',
        'GET_INFO', 'GET_MEMORY', 'GET_STATUS', 'GET_GPS', 'GET_CONFIG', 'SET_CONFIG', 'GET_STATS',
        'GET_AP', 'START_SCAN', 'STOP_SCAN', 'PAUSE_SCAN', 'RESUME_SCAN',
        'RESUME_LINK', 'RESET_STATS', 'NACK'])
    parser.add_argument('arguments', nargs='*')
    args = parser.parse_args()
    if not 1 <= args.request_id <= 0xffffffff:
        parser.error('request_id must be in 1..4294967295')
    sys.stdout.buffer.write(encode(['FBR1', str(args.request_id), args.command, *args.arguments]))


if __name__ == '__main__':
    main()
