from __future__ import annotations

import unittest

from tools.modbus_client.errors import UnsupportedProtocolError
from tools.modbus_gui.backend import ModbusServiceBackend


class FakeTransport:
    def __init__(self, port: str) -> None:
        self.port = port
        self.opened = False
        self.closed = False

    def open(self) -> None:
        self.opened = True

    def close(self) -> None:
        self.closed = True


class FakeClient:
    def __init__(self) -> None:
        self.opened = False
        self.closed = False

    def open(self) -> None:
        self.opened = True

    def close(self) -> None:
        self.closed = True


class FakeService:
    def __init__(
        self,
        address: int,
        *,
        temperature_method: bool,
    ) -> None:
        self.address = address
        self.temperature_method = temperature_method
        self.calls: list[tuple[object, ...]] = []
        if not temperature_method:
            self.read_temperature_snapshot = None

    def identity(self) -> dict[str, object]:
        self.calls.append(("identity",))
        return {
            "device_type": 1,
            "reported_version": "1.8.0",
            "protocol_version": 3,
        }

    def status(self) -> dict[str, object]:
        self.calls.append(("status",))
        return {
            "run_state": "STOPPED",
            "run_state_code": 0,
            "active_config": {
                "valid": True,
                "period_sec": 20,
                "channel_mask": 0x0003,
                "record_count": 7,
            },
        }

    def read_temperature_snapshot(self) -> dict[str, object]:
        self.calls.append(("read_temperature_snapshot",))
        return {
            "sample_id": 9,
            "source": "REAL_DS18B20",
            "sensors": [],
        }

    def config(self, period: int, mask: int, count: int) -> dict[str, object]:
        self.calls.append(("config", period, mask, count))
        return {"period_sec": period, "channel_mask": mask, "record_count": count}

    def apply(self) -> dict[str, object]:
        self.calls.append(("apply",))
        return {"run_state": "STOPPED", "run_state_code": 0}

    def start(self) -> dict[str, object]:
        self.calls.append(("start",))
        return {"run_state": "RUNNING", "run_state_code": 1}

    def stop(self) -> dict[str, object]:
        self.calls.append(("stop",))
        return {"run_state": "STOPPED", "run_state_code": 0}

    def single(self, command_id: int) -> dict[str, object]:
        self.calls.append(("single", command_id))
        return {"run_state": "STOPPED", "run_state_code": 0}

    def storage_status(self) -> dict[str, object]:
        self.calls.append(("storage_status",))
        return {"generated": 12, "synced": 11}


class BackendTests(unittest.TestCase):
    def _backend(self, *, temperature_method: bool = True):
        client = FakeClient()
        service = FakeService(1, temperature_method=temperature_method)
        transport = FakeTransport("/dev/fake")
        backend = ModbusServiceBackend(
            transport_factory=lambda port: transport,
            client_factory=lambda _transport, **_kwargs: client,
            service_factory=lambda _client, address: FakeService(
                address,
                temperature_method=temperature_method,
            ),
        )
        return backend, transport, client

    def test_connect_calls_open_identity_and_status(self) -> None:
        backend = ModbusServiceBackend(
            transport_factory=lambda port: FakeTransport(port),
            client_factory=lambda _transport, **_kwargs: FakeClient(),
            service_factory=lambda _client, address: FakeService(
                address,
                temperature_method=True,
            ),
        )

        result = backend.connect("/dev/fake", 1)

        self.assertEqual(result["identity"]["protocol_version"], 3)
        self.assertEqual(result["status"]["run_state"], "STOPPED")
        backend.close()

    def test_missing_temperature_method_does_not_reuse_test_snapshot(self) -> None:
        backend, _transport, _client = self._backend(temperature_method=False)
        backend.connect("/dev/fake", 1)

        with self.assertRaises(UnsupportedProtocolError) as raised:
            backend.read_temperature_snapshot()

        self.assertTrue(raised.exception.details["interface_unavailable"])
        backend.close()

    def test_poll_and_single_call_temperature_service_method(self) -> None:
        backend, _transport, _client = self._backend()
        backend.connect("/dev/fake", 1)
        service = backend._service
        service.calls.clear()

        polled = backend.poll()
        singled = backend.single_sample(42)

        self.assertEqual(polled["temperature_snapshot"]["sample_id"], 9)
        self.assertEqual(
            polled["temperature_snapshot"]["source"],
            "REAL_DS18B20",
        )
        self.assertEqual(singled["temperature_snapshot"]["sample_id"], 9)
        self.assertEqual(
            service.calls,
            [
                ("status",),
                ("read_temperature_snapshot",),
                ("single", 42),
                ("read_temperature_snapshot",),
            ],
        )
        backend.close()

    def test_start_preserves_active_mask_and_count_through_high_level_calls(
        self,
    ) -> None:
        backend, _transport, _client = self._backend()
        backend.connect("/dev/fake", 1)
        service = backend._service
        service.calls.clear()

        result = backend.start_periodic(30)

        self.assertFalse(result["pending"])
        self.assertEqual(
            service.calls,
            [
                ("status",),
                ("config", 30, 0x0003, 7),
                ("apply",),
                ("start",),
            ],
        )
        self.assertEqual(result["channel_mask"], 0x0003)
        self.assertEqual(result["record_count"], 7)
        backend.close()

    def test_storage_and_stop_delegate_to_existing_service_methods(self) -> None:
        backend, _transport, _client = self._backend()
        backend.connect("/dev/fake", 1)
        service = backend._service
        service.calls.clear()

        storage = backend.refresh_storage()
        stopped = backend.stop_periodic()

        self.assertEqual(storage["synced"], 11)
        self.assertEqual(stopped["run_state_code"], 0)
        self.assertEqual(
            service.calls,
            [("storage_status",), ("stop",)],
        )
        backend.close()


if __name__ == "__main__":
    unittest.main()
