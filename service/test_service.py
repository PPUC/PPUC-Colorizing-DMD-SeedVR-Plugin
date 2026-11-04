# license:GPLv3+
"""Tests of the upscaling service with the stub backend and a stand-in upscaler command.

With SEEDVR_TESTS_BINARY set (ctest sets it), the plugin's test program also runs against the
service, as the plugin talks to it.
"""

import base64
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import seedvr_service  # noqa: E402  pylint: disable=wrong-import-position


def picture(width, height, seed=0):
    rgb = bytearray()
    for index in range(width * height):
        rgb.extend(((index * 7 + seed) % 256, (index * 13) % 256, (index * 29 + seed) % 256))
    return (width, height, rgb)


def frame_json(picture_id, pic):
    width, height, rgb = pic
    return {"id": picture_id, "width": width, "height": height,
            "rgb": base64.b64encode(bytes(rgb)).decode()}


class Running:
    """A service on a free port of 127.0.0.1, for the length of a test."""

    def __init__(self, backend, token="", max_frames=64):
        self.service = seedvr_service.Service(backend, max_frames, token)
        self.server = seedvr_service.make_server(self.service, "127.0.0.1", 0)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.service.stop()

    def call(self, method, path, body=None, token=""):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.url + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        if token:
            request.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                return answer.status, json.loads(answer.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def wait(self, job_id, token=""):
        for _ in range(400):
            status, answer = self.call("GET", f"/v1/jobs/{job_id}", token=token)
            if answer.get("state") not in ("queued", "running"):
                return status, answer
            time.sleep(0.025)
        raise AssertionError("the job did not end")


class PictureTests(unittest.TestCase):
    def test_png_round_trip(self):
        pic = picture(9, 4, 3)
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "p.png")
            seedvr_service.write_png(path, pic)
            self.assertEqual(seedvr_service.read_png(path), pic)

    def test_nearest_and_area_average(self):
        pic = picture(3, 2, 1)
        big = seedvr_service.nearest(pic, 4)
        self.assertEqual(big[:2], (12, 8))
        self.assertEqual(big[2][(5 * 12 + 7) * 3:(5 * 12 + 7) * 3 + 3],
                         pic[2][(1 * 3 + 1) * 3:(1 * 3 + 1) * 3 + 3])
        self.assertEqual(seedvr_service.area_resize(big, 3, 2), pic)
        # Two pixels averaged into one.
        mixed = seedvr_service.area_resize((2, 1, bytearray([0, 0, 0, 255, 100, 51])), 1, 1)
        self.assertEqual(bytes(mixed[2]), bytes([128, 50, 26]))


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.running = Running(seedvr_service.StubBackend(["3b", "7b"]), max_frames=4)

    def tearDown(self):
        self.running.close()

    def test_info(self):
        status, info = self.running.call("GET", "/v1/info")
        self.assertEqual(status, 200)
        self.assertEqual((info["protocol"], info["backend"], info["models"], info["maxFrames"]),
                         (1, "stub", ["3b", "7b"], 4))

    def test_job_doubles_each_picture(self):
        clip = [frame_json("f1", picture(4, 2, 1)), frame_json("f2", picture(4, 2, 2))]
        status, answer = self.running.call("POST", "/v1/jobs", {
            "protocol": 1, "model": "7b", "seed": 3, "scale": 2,
            "sequences": [{"frames": clip}, {"frames": [frame_json("b0", picture(4, 2, 5))]}]})
        self.assertEqual(status, 202)
        _, state = self.running.wait(answer["job"])
        self.assertEqual((state["state"], state["done"], state["total"]), ("done", 3, 3))
        results = {item["id"]: item for item in state["results"]}
        self.assertEqual(sorted(results), ["b0", "f1", "f2"])
        expected = seedvr_service.nearest(picture(4, 2, 2), 2)
        self.assertEqual((results["f2"]["width"], results["f2"]["height"]), (8, 4))
        self.assertEqual(base64.b64decode(results["f2"]["rgb"]), bytes(expected[2]))

    def test_refused_requests(self):
        cases = [
            {"sequences": []},
            {"model": "13b", "sequences": [{"frames": [frame_json("f", picture(2, 2))]}]},
            {"sequences": [{"frames": [{"id": "f", "width": 2, "height": 2, "rgb": "AAAA"}]}]},
            {"sequences": [{"frames": [frame_json(str(i), picture(2, 2)) for i in range(5)]}]},
            {"sequences": [{"frames": [frame_json("a", picture(2, 2)),
                                       frame_json("b", picture(4, 2))]}]},
        ]
        for body in cases:
            status, answer = self.running.call("POST", "/v1/jobs", body)
            self.assertEqual(status, 400, body)
            self.assertIn("error", answer)
        self.assertEqual(self.running.call("GET", "/v1/jobs/nothing")[0], 404)

    def test_cancel(self):
        status, answer = self.running.call("DELETE", "/v1/jobs/nothing")
        self.assertEqual(status, 404)
        _, answer = self.running.call("POST", "/v1/jobs", {
            "sequences": [{"frames": [frame_json("f", picture(2, 2))]}]})
        status, answer = self.running.call("DELETE", f"/v1/jobs/{answer['job']}")
        self.assertEqual(status, 200)
        self.assertIn(answer["state"], ("cancelled", "running", "done"))


class TokenTests(unittest.TestCase):
    def test_token_is_needed(self):
        running = Running(seedvr_service.StubBackend(["3b"]), token="secret")
        try:
            self.assertEqual(running.call("GET", "/v1/info")[0], 401)
            self.assertEqual(running.call("GET", "/v1/info", token="wrong")[0], 401)
            self.assertEqual(running.call("GET", "/v1/info", token="secret")[0], 200)
        finally:
            running.close()


# A stand-in for the upscaler: enlarges each PNG of the input folder by {scale} (nearest) into
# the output folder, as SeedVR's script writes its pictures.
FAKE_UPSCALER = textwrap.dedent("""
    import glob, os, sys
    sys.path.insert(0, sys.argv[4])
    import seedvr_service as s
    source, target, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
    for path in sorted(glob.glob(os.path.join(source, "*.png"))):
        s.write_png(os.path.join(target, os.path.basename(path)),
                    s.nearest(s.read_png(path), scale))
""")


class CommandBackendTests(unittest.TestCase):
    def test_clip_through_a_command(self):
        with tempfile.TemporaryDirectory() as folder:
            script = os.path.join(folder, "fake_upscaler.py")
            with open(script, "w", encoding="utf-8") as file:
                file.write(FAKE_UPSCALER)
            here = os.path.dirname(os.path.abspath(__file__))
            command = f'"{sys.executable}" "{script}" {{input}} {{output}} {{scale}} "{here}"'
            backend = seedvr_service.CommandBackend(["3b"], command, 2, "png", "ffmpeg", folder)
            clip = [picture(4, 2, 1), picture(4, 2, 9)]
            results = backend.upscale(clip, 2, "3b", 1, lambda: False)
            # Enlarged 2x before, 4x by the command, scaled down to 2x: nearest 2x.
            self.assertEqual([r[:2] for r in results], [(8, 4), (8, 4)])
            self.assertEqual(results[1], seedvr_service.nearest(clip[1], 2))

    def test_a_failing_command_fails_the_clip(self):
        backend = seedvr_service.CommandBackend(["3b"], f'"{sys.executable}" -c "exit(3)"', 1,
                                                "png", "ffmpeg", None)
        with self.assertRaises(RuntimeError):
            backend.upscale([picture(2, 2)], 2, "3b", 1, lambda: False)


@unittest.skipUnless(os.environ.get("SEEDVR_TESTS_BINARY"), "SEEDVR_TESTS_BINARY not set")
class PluginClientTests(unittest.TestCase):
    def test_plugin_client_against_the_service(self):
        running = Running(seedvr_service.StubBackend(["3b", "7b"]), token="t0ken", max_frames=3)
        try:
            result = subprocess.run([os.environ["SEEDVR_TESTS_BINARY"], "--service", running.url,
                                     "t0ken"], capture_output=True, text=True, timeout=120,
                                    check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            running.close()


if __name__ == "__main__":
    unittest.main()
