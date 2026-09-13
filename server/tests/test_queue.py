"""Pending-attach queue: enqueue, auto-dequeue, hand-off, goodbye cleanup.

The queue is what lets a second client see a device that somebody else is
already using and wait for it instead of getting "device busy". These tests
pin the invariants that make the hand-off safe:

  * a client that actually holds a device is never left in the waitlist;
  * only the head of a free device is woken, and only once;
  * a client that exits stops occupying a queue slot (no phantom waiter);
  * names shown to the UI come from the heartbeat registry, not the queue.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import web


class QueueTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.old = {
            name: getattr(web, name)
            for name in ("CLIENTS_FILE", "QUEUE_FILE", "NOTIFY_FILE", "MANAGED_FILE")
        }
        web.CLIENTS_FILE = root / "clients.json"
        web.QUEUE_FILE = root / "queue.json"
        web.NOTIFY_FILE = root / "queue-notify.json"
        web.MANAGED_FILE = root / "managed-busids"

        # The container decides "is this busid exported" from sysfs; on the
        # test host there is no sysfs, so fake it as always-exported.
        self.old_is_shared = web.is_shared
        web.is_shared = lambda busid: True

    def tearDown(self):
        for name, value in self.old.items():
            setattr(web, name, value)
        web.is_shared = self.old_is_shared
        self.temp.cleanup()

    @staticmethod
    def notices(entries):
        """Compare on (busid, action) only — `at` is a timestamp."""
        return [(item["busid"], item["action"]) for item in entries]

    def heartbeat(self, client_id, name, busids, **extra):
        payload = {
            "clientId": client_id,
            "clientName": name,
            "busids": busids,
            "dataPort": 5555,
        }
        payload.update(extra)
        ok, message, record, notifications = web.update_client(payload, "192.168.1.10")
        self.assertTrue(ok, message)
        return record, notifications

    # -- basic FIFO ---------------------------------------------------------

    def test_enqueue_keeps_order_and_moves_self_to_tail(self):
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.assertEqual(web.queue_for("1-1"), ["A", "B"])

        # Re-requesting the device means "as soon as possible after I'm done",
        # so the repeat entry moves to the tail rather than duplicating.
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1"])
        self.assertEqual(web.queue_for("1-1"), ["B", "A"])

    def test_dequeue_removes_only_self(self):
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.heartbeat("A", "甲机", [], dequeueBusids=["1-1"])
        self.assertEqual(web.queue_for("1-1"), ["B"])

    def test_holding_a_device_removes_you_from_its_queue(self):
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1"])
        self.assertEqual(web.queue_for("1-1"), ["A"])
        # The attach succeeded, so the next heartbeat reports the busid.
        self.heartbeat("A", "甲机", ["1-1"])
        self.assertEqual(web.queue_for("1-1"), [])

    def test_bad_queue_payload_is_rejected(self):
        ok, message, record, notifications = web.update_client(
            {"clientId": "A", "clientName": "甲机", "busids": [], "dataPort": 5555,
             "enqueueBusids": "1-1"},
            "192.168.1.10",
        )
        self.assertFalse(ok)
        self.assertIn("排队", message)

    # -- hand-off -----------------------------------------------------------

    def test_holder_release_wakes_only_the_head_once(self):
        self.heartbeat("A", "甲机", ["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.heartbeat("C", "丙机", [], enqueueBusids=["1-1"])
        # While A holds the device nobody is woken.
        self.assertEqual(web.pop_queue_notifications_for("B"), [])
        self.assertEqual(web.pop_queue_notifications_for("C"), [])

        # A leaves: its record and claim go away, then the queue advances.
        web.handle_client_goodbye("A", ["1-1"])
        self.assertEqual(web.queue_for("1-1"), ["B", "C"])
        self.assertEqual(self.notices(web.pop_queue_notifications_for("B")), [("1-1", "attach")])
        # B was woken once; C is still waiting for its own turn.
        self.assertEqual(web.pop_queue_notifications_for("B"), [])
        self.assertEqual(web.pop_queue_notifications_for("C"), [])

    def test_notification_is_consumed_by_the_next_heartbeat(self):
        self.heartbeat("A", "甲机", ["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        web.handle_client_goodbye("A", ["1-1"])
        # The heartbeat that carries the notice drains the mailbox...
        _record, notifications = self.heartbeat("B", "乙机", [])
        self.assertEqual(self.notices(notifications), [("1-1", "attach")])
        # ...and it is not replayed afterwards.
        _record, notifications = self.heartbeat("B", "乙机", [])
        self.assertEqual(notifications, [])

    def test_unshare_notifies_every_waiter_with_dropped(self):
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.heartbeat("C", "丙机", [], enqueueBusids=["1-1"])
        web.queue_notify_busid_holders("1-1")
        web.queue_clear("1-1")
        self.assertEqual(self.notices(web.pop_queue_notifications_for("B")), [("1-1", "dropped")])
        self.assertEqual(self.notices(web.pop_queue_notifications_for("C")), [("1-1", "dropped")])
        self.assertEqual(web.queue_for("1-1"), [])

    def test_head_is_notified_once_per_free_window(self):
        self.heartbeat("A", "甲机", ["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        web.handle_client_goodbye("A", ["1-1"])
        self.assertEqual(self.notices(web.pop_queue_notifications_for("B")), [("1-1", "attach")])
        # B keeps re-sending its queue request every heartbeat while it waits;
        # that must not turn into a wakeup on every single one.
        _record, notifications = self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.assertEqual(self.notices(notifications), [])
        _record, notifications = self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.assertEqual(self.notices(notifications), [])

    def test_head_is_renotified_after_someone_else_takes_the_device(self):
        self.heartbeat("A", "甲机", ["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.heartbeat("C", "丙机", [], enqueueBusids=["1-1"])
        web.handle_client_goodbye("A", ["1-1"])
        self.assertEqual(self.notices(web.pop_queue_notifications_for("B")), [("1-1", "attach")])

        # C grabs the device before B gets to it, then releases it. B is still
        # the head and must be woken again — the previous memo must not stick.
        self.heartbeat("C", "丙机", ["1-1"])
        web.handle_client_goodbye("C", ["1-1"])
        self.assertEqual(self.notices(web.pop_queue_notifications_for("B")), [("1-1", "attach")])

    # -- no phantom waiters -------------------------------------------------

    def test_goodbye_removes_the_client_from_every_queue(self):
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1", "1-2"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        web.handle_client_goodbye("A", [])
        self.assertEqual(web.queue_for("1-1"), ["B"])
        self.assertEqual(web.queue_for("1-2"), [])

    def test_goodbye_with_shutdown_flag_leaves_no_queue_entry(self):
        ok, message, record, notifications = web.update_client(
            {"clientId": "A", "clientName": "甲机", "busids": ["1-1"], "dataPort": 5555,
             "shutdown": True, "enqueueBusids": ["1-1"]},
            "192.168.1.10",
        )
        self.assertTrue(ok, message)
        # The enqueue in the same payload must not resurrect the departing
        # client — otherwise the UI would show a waiter that can never answer.
        self.assertEqual(web.queue_for("1-1"), [])
        self.assertEqual(web.read_clients(), [])

    # -- UI projection ------------------------------------------------------

    def test_queue_info_resolves_display_names(self):
        self.heartbeat("A", "甲机", [], enqueueBusids=["1-1"])
        self.heartbeat("B", "乙机", [], enqueueBusids=["1-1"])
        self.assertEqual(
            web.queue_info("1-1"),
            [{"clientId": "A", "name": "甲机"}, {"clientId": "B", "name": "乙机"}],
        )
        # An unknown device has an empty (never missing) waitlist.
        self.assertEqual(web.queue_info("1-9"), [])
        # A clientId with no live registration still renders, with a fallback.
        web.queue_append("1-1", "ghost")
        self.assertEqual(web.queue_info("1-1")[-1], {"clientId": "ghost", "name": "未命名客户端"})

    def test_legacy_wrapped_file_still_loads(self):
        web.QUEUE_FILE.write_text('{"version": 1, "queues": {"1-1": ["A", "B"]}}', encoding="utf-8")
        self.assertEqual(web.queue_for("1-1"), ["A", "B"])


if __name__ == "__main__":
    unittest.main()
