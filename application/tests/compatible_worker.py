"""Compatible-copy consumers use temporary fixtures only; no NAS or user media."""
import hashlib
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave

EXE = Path(sys.argv.pop(1)).resolve()


def brief(events):
    return [{key: value for key, value in event.items() if key != 'data'} for event in events]


def version_key(library, relative, size, modified):
    # Independent implementation of the documented QDataStream cache identity.
    def string(value):
        data = value.encode('utf-16-be')
        return struct.pack('>I', len(data)) + data
    body = b''.join(string(value) for value in
                    ('envelope-v1-per-channel-2048', library, relative))
    return hashlib.sha256(body + struct.pack('>qq', size, modified)).hexdigest()


class CompatibleWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='compatible-worker-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.copy = self.root / 'safe-copy.wav'
        with wave.open(str(self.copy), 'wb') as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(48000)
            target.writeframes(b'\x20\x10' * (48000 * 3))
        self.bytes = self.copy.read_bytes()
        self.original_stat = self.copy.stat()
        self.config = dict(path=str(self.copy), compatiblePath=str(self.copy),
                           compatibleSha256=hashlib.sha256(self.bytes).hexdigest(),
                           compatibleSize=len(self.bytes),
                           compatibleModified=self.original_stat.st_mtime_ns // 1_000_000,
                           indexedSize=len(self.bytes), indexedModified=1000,
                           library='original-library', relative='日本語/原曲.wav',
                           local=str(self.root / 'local'), shared=str(self.root / 'shared'),
                           verificationCache=str(self.root / 'media-verification.sqlite'))

    def run_wave(self, config=None):
        result = subprocess.run([str(EXE), '--worker', 'wave'],
                                input=json.dumps(config or self.config) + '\n',
                                text=True, capture_output=True, timeout=30)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        return result.returncode, events

    def tamper_middle_with_same_metadata(self):
        changed = bytearray(self.bytes)
        changed[len(changed) // 2] ^= 0x40
        self.copy.write_bytes(changed)
        os.utime(self.copy, ns=(self.original_stat.st_atime_ns, self.original_stat.st_mtime_ns))
        self.assertEqual(self.copy.stat().st_size, self.original_stat.st_size)
        self.assertEqual(self.copy.stat().st_mtime_ns, self.original_stat.st_mtime_ns)

    def prepare_large_copy(self):
        with wave.open(str(self.copy), 'wb') as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(48000)
            target.writeframes(b'\x20\x10' * (4 * 1024 * 1024))
        self.bytes = self.copy.read_bytes()
        self.original_stat = self.copy.stat()
        self.config.update(compatibleSha256=hashlib.sha256(self.bytes).hexdigest(),
                           compatibleSize=len(self.bytes), indexedSize=len(self.bytes),
                           compatibleModified=self.original_stat.st_mtime_ns // 1_000_000)

    def test_wave_hashes_once_then_reuses_receipt_across_workers(self):
        self.prepare_large_copy()
        for cached in (False, True):
            with self.subTest(cached=cached):
                status, events = self.run_wave()
                self.assertEqual(status, 0, brief(events))
                verification = [(index, event) for index, event in enumerate(events)
                                if event.get('event') == 'reading'
                                and event.get('phase') == 'verification']
                self.assertGreaterEqual(len(verification), 2, brief(events))
                # Initial and stability guards still run. A receipt hit is a zero
                # heartbeat, never a positive audio read or a completion receipt.
                self.assertGreaterEqual(sum(event['bytes'] == 0 for _, event in verification), 2)
                self.assertTrue(all('complete' not in event and 'milliseconds' not in event
                                    for _, event in verification))
                positions = [event['bytes'] for _, event in verification]
                self.assertEqual(positions[0], 0)
                if cached:
                    self.assertEqual(max(positions), 0, brief(events))
                else:
                    self.assertEqual(max(positions), len(self.bytes))
                    positive_steps = [later - earlier for earlier, later in zip(positions, positions[1:])
                                      if later > earlier]
                    self.assertTrue(positive_steps)
                    self.assertLessEqual(max(positive_steps), 1024 * 1024)
                completed = next(index for index, event in enumerate(events)
                                 if event.get('event') == 'wave' and event.get('complete'))
                self.assertLess(verification[-1][0], completed)

    def test_player_reports_verification_progress_before_ready(self):
        self.prepare_large_copy()
        player = subprocess.Popen([str(EXE), '--worker', 'player'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding='utf-8')
        events = queue.Queue()
        def read():
            for line in player.stdout:
                if line.startswith('{'):
                    events.put(json.loads(line))
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        try:
            command = dict(self.config, op='load', generation=19, autoplay=False, volume=0)
            player.stdin.write(json.dumps(command) + '\n')
            player.stdin.flush()
            observed = []
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                try:
                    event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                except queue.Empty:
                    break
                observed.append(event)
                if event.get('event') in ('error', 'ready'):
                    break
            self.assertEqual(observed[-1].get('event') if observed else None, 'ready', brief(observed))
            verification = [event for event in observed if event.get('event') == 'verifying']
            self.assertGreater(len(verification), 2, brief(observed))
            self.assertTrue(all(event.get('generation') == 19 for event in verification), brief(observed))
            positions = [event['bytes'] for event in verification]
            self.assertEqual(positions[0], 0)
            self.assertEqual(positions[-1], len(self.bytes))
            self.assertEqual(positions, sorted(positions))
            self.assertLessEqual(max(later - earlier for earlier, later in zip(positions, positions[1:])),
                                 1024 * 1024)
            self.assertTrue(all('complete' not in event and 'duration' not in event
                                for event in verification))
        finally:
            player.stdin.close()
            try:
                player.wait(timeout=5)
            except subprocess.TimeoutExpired:
                player.kill()
                player.wait()
            reader.join(timeout=2)
            player.stdout.close()
            player.stderr.close()

    def test_different_copy_timestamp_saves_view_under_original_version(self):
        for indexed_size in (len(self.bytes), 0):
            with self.subTest(indexed_size=indexed_size):
                config = dict(self.config, indexedSize=indexed_size)
                status, events = self.run_wave(config)
                self.assertEqual(status, 0, brief(events))
                self.assertTrue(any(event.get('complete') and event.get('stored')
                                    for event in events), brief(events))
                key = version_key('original-library', '日本語/原曲.wav', indexed_size, 1000)
                self.assertTrue((self.root / 'local' / 'views-v1' / key[:2] / (key + '.view')).is_file())
                # A queued shared-publication request becomes verified local work for copies.
                status, events = self.run_wave(dict(config, operation='publish_existing'))
                self.assertEqual(status, 0, brief(events))
                self.assertTrue(any(event.get('complete') and event.get('stored')
                                    for event in events), brief(events))
                self.assertFalse(any(event.get('published') for event in events), brief(events))
        self.assertFalse((self.root / 'shared').exists(), 'compatible media must not publish original caches')
        self.assertEqual(self.copy.read_bytes(), self.bytes)

    def test_middle_byte_change_rejects_before_cache_reuse(self):
        self.run_wave()
        self.tamper_middle_with_same_metadata()
        status, events = self.run_wave()
        self.assertNotEqual(status, 0, brief(events))
        self.assertTrue(any(event.get('event') == 'error' and event.get('kind') == 'compatible_unavailable'
                            for event in events), brief(events))
        self.assertFalse(any(event.get('event') == 'wave' for event in events), brief(events))

    def test_publish_existing_cannot_bypass_full_copy_verification(self):
        self.run_wave()
        self.tamper_middle_with_same_metadata()
        config = dict(self.config, operation='publish_existing')
        status, events = self.run_wave(config)
        self.assertNotEqual(status, 0, brief(events))
        self.assertTrue(any(event.get('event') == 'error' and event.get('kind') == 'compatible_unavailable'
                            for event in events), brief(events))
        self.assertFalse(any(event.get('event') == 'wave' for event in events), brief(events))
        self.assertFalse((self.root / 'shared').exists())

    def test_player_rejects_copy_changed_in_middle(self):
        self.tamper_middle_with_same_metadata()
        player = subprocess.Popen([str(EXE), '--worker', 'player'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding='utf-8')
        events = queue.Queue()
        def read():
            for line in player.stdout:
                if line.startswith('{'):
                    events.put(json.loads(line))
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        try:
            command = dict(self.config, op='load', generation=18, autoplay=False, volume=0)
            player.stdin.write(json.dumps(command) + '\n')
            player.stdin.flush()
            observed = []
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                try:
                    event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                except queue.Empty:
                    break
                observed.append(event)
                if event.get('event') in ('error', 'ready'):
                    break
            self.assertTrue(any(event.get('event') == 'error' and event.get('generation') == 18
                                and event.get('kind') == 'compatible_unavailable' for event in observed), observed)
            self.assertFalse(any(event.get('event') == 'ready' for event in observed), observed)
            # Restoring the verified copy permits Play to recover without switching rows.
            self.copy.write_bytes(self.bytes)
            os.utime(self.copy, ns=(self.original_stat.st_atime_ns, self.original_stat.st_mtime_ns))
            player.stdin.write(json.dumps(dict(op='play', generation=18)) + '\n')
            player.stdin.flush()
            deadline = time.monotonic() + 6
            ready = False
            while time.monotonic() < deadline:
                try:
                    event = events.get(timeout=max(0.01, deadline - time.monotonic()))
                except queue.Empty:
                    break
                observed.append(event)
                if event.get('event') == 'ready' and event.get('generation') == 18:
                    ready = True
                    break
            self.assertTrue(ready, observed)
        finally:
            player.stdin.close()
            try:
                player.wait(timeout=5)
            except subprocess.TimeoutExpired:
                player.kill()
                player.wait()
            reader.join(timeout=2)
            player.stdout.close()
            player.stderr.close()


if __name__ == '__main__':
    unittest.main()
