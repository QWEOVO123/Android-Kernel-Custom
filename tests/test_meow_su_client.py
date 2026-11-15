"""Exercise the standard su command line against a local protocol stub."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'tools/meow-su-client.c'


@unittest.skipUnless(os.name == 'posix' and os.geteuid() == 0 and shutil.which('cc'),
                     'requires a root-owned local socket and a C compiler')
class SuClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='meow-su-test-')
        cls.addClassCleanup(cls.directory.cleanup)
        root = Path(cls.directory.name)
        cls.socket_path = root / 'socket'
        cls.binary = root / 'su'
        source = root / 'client.c'
        source.write_text(SOURCE.read_text().replace('/dev/.meow-manager/su', str(cls.socket_path)))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        str(source), '-o', str(cls.binary)], check=True)

    def request(self, args, payload, allowed):
        self.socket_path.unlink(missing_ok=True)
        with socket.socket(socket.AF_UNIX) as server, ThreadPoolExecutor(1) as workers:
            server.bind(str(self.socket_path))
            server.listen(1)
            server.settimeout(3)

            def reply():
                peer, _ = server.accept()
                with peer, peer.makefile('rb') as stream:
                    peer.settimeout(3)
                    peer.sendall(struct.pack('!i', 0 if allowed else 13))
                    if not allowed:
                        return stream.read(1)
                    magic, size = struct.unpack('!II', stream.read(8))
                    self.assertEqual(magic, 0x4D535531)
                    command = stream.read(size)
                    data = stream.read()
                    peer.sendall(struct.pack('!i', 5) + b'uid=0' + struct.pack('!ii', -1, 7))
                    return command, data

            received = workers.submit(reply)
            result = subprocess.run([str(self.binary), *args], input=payload,
                                    capture_output=True, timeout=5)
            return result, received.result()

    def test_one_shot_command_and_exit_status(self):
        result, request = self.request(['-c', 'id'], b'', True)
        self.assertEqual(request, (b'id', b''))
        self.assertEqual((result.stdout, result.returncode), (b'uid=0', 7))

    def test_plain_su_forwards_shell_input(self):
        result, request = self.request([], b'id\nexit\n', True)
        self.assertEqual(request, (b'', b'id\nexit\n'))
        self.assertEqual((result.stdout, result.returncode), (b'uid=0', 7))

    def test_denied_client_sends_no_command(self):
        result, request = self.request(['-c', 'id'], b'', False)
        self.assertEqual(request, b'')
        self.assertEqual(result.returncode, 126)


if __name__ == '__main__':
    unittest.main()
