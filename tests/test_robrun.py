"""Изолированные тесты robrun.py (без Windows, без игры).

Запуск: python -m unittest discover -s tests -v
"""

import http.client
import importlib.util
import json
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("robrun", ROOT / "robrun.py")
robrun = importlib.util.module_from_spec(spec)
spec.loader.exec_module(robrun)


class TestPure(unittest.TestCase):
    def test_bootstrap(self):
        payload = robrun.bootstrap("a" * 64)
        self.assertTrue(payload.startswith(b"ROBRUN/1\nhttp://127.0.0.1:800\n1\n"))
        self.assertTrue(payload.endswith(b"a" * 64 + b"\n"))

    def test_bootstrap_rejects_bad_ticket(self):
        with self.assertRaises(ValueError):
            robrun.bootstrap("bad")
        with self.assertRaises(ValueError):
            robrun.bootstrap("A" * 64)  # uppercase

    def test_frame(self):
        framed = robrun.frame(b"hi")
        self.assertEqual(len(framed), 6)
        self.assertEqual(framed[4:], b"hi")
        with self.assertRaises(ValueError):
            robrun.frame(b"")
        with self.assertRaises(ValueError):
            robrun.frame(b"x" * (robrun.MAX_FRAME + 1))

    def test_verify_current_exe(self):
        data = (ROOT / "RobRun.exe").read_bytes()
        robrun.verify_local_image(data)  # не должен бросать

    def test_verify_rejects_garbage(self):
        with self.assertRaises(ValueError):
            robrun.verify_local_image(b"not an exe")

    def test_validate_config(self):
        robrun.validate_config(b"[Keys]\nMenuPrimary=45\n")
        robrun.validate_config(b"")  # пустой конфиг допустим (первый запуск)
        with self.assertRaises(ValueError):
            robrun.validate_config(b"no equals here\n")
        with self.assertRaises(ValueError):
            robrun.validate_config(b"a\x00b")

    def test_pipe_path(self):
        self.assertEqual(robrun.pipe_path("RobRun." + "a" * 32),
                         "\\\\.\\pipe\\RobRun." + "a" * 32)
        with self.assertRaises(ValueError):
            robrun.pipe_path("bad")


class TestHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.srv = robrun.LocalServer(0, cls.tmp)
        cls.port = cls.srv.server_port
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()
        time.sleep(0.3)

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data

    def test_check_server(self):
        robrun.check_server(self.port)

    def test_redeem_and_config(self):
        status, data = self.req("POST", "/api/v1/runtime/redeem", body=b"b" * 64)
        self.assertEqual(status, 200)
        token = data.decode()
        self.assertEqual(len(token), 64)

        status, _ = self.req("GET", "/api/v1/runtime/config")
        self.assertEqual(status, 401)

        auth = {"Authorization": f"Bearer {token}"}
        status, data = self.req("GET", "/api/v1/runtime/config", headers=auth)
        self.assertEqual(status, 200)

        status, _ = self.req("PUT", "/api/v1/runtime/config",
                             body=b"[Keys]\nMenuPrimary=45\n", headers=auth)
        self.assertEqual(status, 204)

        status, data = self.req("GET", "/api/v1/runtime/config", headers=auth)
        self.assertEqual(status, 200)
        self.assertIn(b"MenuPrimary=45", data)

    def test_redeem_bad_ticket(self):
        status, _ = self.req("POST", "/api/v1/runtime/redeem", body=b"bad")
        self.assertEqual(status, 400)

    def test_crash(self):
        status, data = self.req("POST", "/api/v1/runtime/redeem", body=b"c" * 64)
        token = data.decode()
        auth = {"Authorization": f"Bearer {token}"}
        body = json.dumps({"version": "test", "code": "DIAG", "mod": "x"}).encode()
        status, _ = self.req("POST", "/api/v1/runtime/crash", body=body, headers=auth)
        self.assertEqual(status, 204)

    def test_unknown_and_method(self):
        status, _ = self.req("GET", "/api/v1/anticheat/status")
        self.assertEqual(status, 404)
        status, _ = self.req("GET", "/api/v1/runtime/redeem")
        self.assertEqual(status, 405)


if __name__ == "__main__":
    unittest.main()
