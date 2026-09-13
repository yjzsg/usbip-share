"""Heartbeat claims are display-only and never replace the observed peer."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web


class PublicIpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        # Redirect every persisted store the heartbeat touches, so the test
        # never writes to the container paths and never inherits state.
        self.old = {
            name: getattr(web, name)
            for name in ("CLIENTS_FILE", "QUEUE_FILE", "NOTIFY_FILE")
        }
        web.CLIENTS_FILE = root / "clients.json"
        web.QUEUE_FILE = root / "queue.json"
        web.NOTIFY_FILE = root / "queue-notify.json"

    def tearDown(self):
        for name, value in self.old.items():
            setattr(web, name, value)
        self.temp.cleanup()

    def register(self, public_ip=None):
        payload = {"clientId": "sample", "clientName": "Sample PC", "busids": ["1-1"], "dataPort": 5555}
        if public_ip is not None:
            payload["publicIp"] = public_ip
        ok, message, record, _notifications = web.update_client(payload, "192.168.1.25")
        self.assertTrue(ok, message)
        self.assertEqual(record["address"], "192.168.1.25")
        return record

    def test_ipv4_claim_keeps_peer(self):
        self.assertEqual(self.register("8.8.8.8")["publicIp"], "8.8.8.8")

    def test_ipv6_normalized(self):
        self.assertEqual(self.register("2606:4700:4700:0:0:0:0:1111")["publicIp"], "2606:4700:4700::1111")

    def test_invalid_claims(self):
        for value in ("127.0.0.1", "10.0.0.1", "192.168.1.2", "203.0.113.1", "224.1.1.1", "ff02::1", "::1", "fe80::1%eth0", "<script>", "8.8.8.8\r\nInjected: yes", [], {}, 1):
            with self.subTest(value=value):
                self.assertEqual(self.register(value)["publicIp"], "")

    def test_old_clients(self):
        self.assertEqual(self.register()["publicIp"], "")

    def test_presence_roundtrip(self):
        self.register("8.8.8.8")
        self.assertEqual(web.read_clients()[0]["publicIp"], "8.8.8.8")
        peer = web.connection_info("1-1")[0]
        self.assertEqual(peer["address"], "192.168.1.25")
        self.assertEqual(peer["publicIp"], "8.8.8.8")


if __name__ == "__main__":
    unittest.main()
