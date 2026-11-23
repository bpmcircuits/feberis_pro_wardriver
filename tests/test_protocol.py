"""Independent CSV reader and protocol reference checks against the C implementation."""
import csv
import io
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from protocol import encode, decode

CORE_EXECUTABLE = sys.argv.pop(1)


class ProtocolTests(unittest.TestCase):
    def test_frame_roundtrip_and_corruption(self):
        fields = ['FBR1', '1', 'START', '2000']
        self.assertEqual(decode(encode(fields)), fields)
        c_frame = subprocess.check_output([CORE_EXECUTABLE, '--frame'])
        self.assertEqual(c_frame, encode(fields))
        self.assertEqual(decode(c_frame), fields)
        frame = bytearray(encode(fields))
        frame[6] ^= 1
        with self.assertRaises(ValueError):
            decode(bytes(frame))

    def test_csv_export_with_quotes_and_binary_ssid(self):
        text = subprocess.check_output([CORE_EXECUTABLE, '--csv'], text=True)
        metadata, rest = text.split('\n', 1)
        self.assertTrue(metadata.startswith('WigleWifi-1.4,'))
        self.assertIn('brand=Sapsan', metadata.split(','))
        rows = list(csv.DictReader(io.StringIO(rest)))
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(len(row), 11)
        self.assertEqual(row['SSID'], 'a,"b\\x0A\\xFF')
        self.assertEqual(row['MAC'], '12:34:56:78:9A:BC')
        self.assertEqual(row['FirstSeen'], '2026-11-21 12:35:19')
        self.assertAlmostEqual(float(row['CurrentLatitude']), 48.1173)
        self.assertEqual(row['AccuracyMeters'], '')
        self.assertEqual(row['Type'], 'WIFI')

    def test_arbitrary_field_injection_rejected(self):
        for bad in ['new\nline', 'tab\tfield', 'x*y', '\x00']:
            with self.assertRaises(ValueError):
                encode(['FBR1', '1', bad])


if __name__ == '__main__':
    unittest.main()
