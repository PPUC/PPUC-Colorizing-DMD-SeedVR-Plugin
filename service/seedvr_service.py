#!/usr/bin/env python3
# license:GPLv3+
"""The upscaling service the SeedVR plugin of the ColorizingDMD editor talks to.

A small HTTP service (protocol 1, see the README): the plugin sends clips of small RGB
pictures, the service upscales each clip and answers with pictures of exactly `scale` times the
size. Jobs run one after the other on one worker thread, so that a GPU holds one model at a
time.

Backends:

- `stub`: nearest-neighbour upscaling, for tests and for trying the plugin without a GPU.
- `command`: runs an upscaler command per clip, for SeedVR2 (see the README for the commands
  of SeedVR's own inference script and of other front ends). The clip goes to the command as
  numbered PNG files (or a lossless video with --input-format mp4), first enlarged by
  --prescale (nearest), as SeedVR2 works better on larger pictures than 128x32; the command
  writes PNG files (or one video) into the output folder, which are scaled down (area average)
  to the size asked for.

Only the Python standard library is needed; ffmpeg only for --input-format mp4 or a command
that writes a video.
"""

import argparse
import base64
import glob
import hmac
import json
import os
import shlex
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import uuid
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL = 1
SERVICE = "colorizingdmd-seedvr"


class RequestError(Exception):
    """A request the service refuses (HTTP 400)."""


class Cancelled(Exception):
    pass


# ---- Pictures --------------------------------------------------------------------------------
# A picture is (width, height, bytearray of RGB, 3 bytes a pixel).


def nearest(picture, factor):
    width, height, rgb = picture
    out = bytearray(width * factor * height * factor * 3)
    row_bytes = width * factor * 3
    for y in range(height):
        row = bytearray(row_bytes)
        for x in range(width):
            pixel = rgb[(y * width + x) * 3:(y * width + x) * 3 + 3]
            for i in range(factor):
                start = (x * factor + i) * 3
                row[start:start + 3] = pixel
        for i in range(factor):
            start = (y * factor + i) * row_bytes
            out[start:start + row_bytes] = row
    return (width * factor, height * factor, out)


def area_resize(picture, out_width, out_height):
    """Scales a picture down (or up) to the size, each target pixel the average of the source
    area it covers, weighted by how much of each source pixel it covers."""
    width, height, rgb = picture
    if (width, height) == (out_width, out_height):
        return picture

    def spans(source, target):
        # For each target pixel: [(source index, weight)], the weights summing to 1.
        result = []
        ratio = source / target
        for t in range(target):
            start = t * ratio
            end = start + ratio
            parts = []
            s = int(start)
            while s < end and s < source:
                covered = min(end, s + 1) - max(start, s)
                if covered > 1e-9:
                    parts.append((s, covered / ratio))
                s += 1
            result.append(parts)
        return result

    xs = spans(width, out_width)
    ys = spans(height, out_height)
    out = bytearray(out_width * out_height * 3)
    for ty, yparts in enumerate(ys):
        for tx, xparts in enumerate(xs):
            sums = [0.0, 0.0, 0.0]
            for sy, wy in yparts:
                base = sy * width
                for sx, wx in xparts:
                    weight = wx * wy
                    at = (base + sx) * 3
                    sums[0] += rgb[at] * weight
                    sums[1] += rgb[at + 1] * weight
                    sums[2] += rgb[at + 2] * weight
            at = (ty * out_width + tx) * 3
            for c in range(3):
                out[at + c] = max(0, min(255, int(round(sums[c]))))
    return (out_width, out_height, out)


def write_png(path, picture):
    width, height, rgb = picture
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * width * 3:(y + 1) * width * 3])

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        file.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        file.write(chunk(b"IEND", b""))


def read_png(path):
    """8-bit RGB, RGBA, grey or grey+alpha PNGs, not interlaced; alpha is dropped."""
    with open(path, "rb") as file:
        data = file.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG file")
    pos = 8
    width = height = None
    color_type = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0 or color_type not in (0, 2, 4, 6):
                raise ValueError(f"{path}: only 8-bit, not interlaced RGB(A) or grey PNGs")
        elif kind == b"IDAT":
            idat.extend(body)
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    previous = bytearray(stride)
    rgb = bytearray(width * height * 3)
    pos = 0
    for y in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[i] = (line[i] + predictor) & 0xFF
        for x in range(width):
            at = x * channels
            if channels < 3:
                rgb[(y * width + x) * 3:(y * width + x) * 3 + 3] = bytes([line[at]] * 3)
            else:
                rgb[(y * width + x) * 3:(y * width + x) * 3 + 3] = line[at:at + 3]
        previous = line
    return (width, height, rgb)


# ---- Backends --------------------------------------------------------------------------------


class StubBackend:
    """Nearest-neighbour upscaling."""

    name = "stub"

    def __init__(self, models):
        self.models = models

    def upscale(self, clip, scale, model, seed, cancelled):
        results = []
        for picture in clip:
            if cancelled():
                raise Cancelled()
            results.append(nearest(picture, scale))
        return results


class CommandBackend:
    """Runs an upscaler command per clip (see the module's text)."""

    name = "command"

    def __init__(self, models, command, prescale, input_format, ffmpeg, work_dir):
        self.models = models
        self.command = command
        self.prescale = max(1, prescale)
        self.input_format = input_format
        self.ffmpeg = ffmpeg
        self.work_dir = work_dir

    def upscale(self, clip, scale, model, seed, cancelled):
        width, height = clip[0][0], clip[0][1]
        target_width, target_height = width * scale, height * scale
        with tempfile.TemporaryDirectory(prefix="seedvr-", dir=self.work_dir) as folder:
            in_dir = os.path.join(folder, "in")
            out_dir = os.path.join(folder, "out")
            os.makedirs(in_dir)
            os.makedirs(out_dir)
            for number, picture in enumerate(clip):
                write_png(os.path.join(in_dir, f"frame_{number:05d}.png"),
                          nearest(picture, self.prescale))
            source = in_dir
            if self.input_format == "mp4":
                source = os.path.join(in_dir, "clip.mp4")
                self._run([self.ffmpeg, "-loglevel", "error", "-y", "-framerate", "25", "-i",
                           os.path.join(in_dir, "frame_%05d.png"), "-c:v", "libx264rgb",
                           "-crf", "0", "-pix_fmt", "rgb24", source], cancelled)
            # The size the upscaler is asked for: the target times the enlargement.
            values = {"input": source, "output": out_dir, "model": model, "seed": str(seed),
                      "width": str(target_width * self.prescale),
                      "height": str(target_height * self.prescale),
                      "count": str(len(clip)), "scale": str(scale * self.prescale)}
            arguments = [part.format(**values) for part in shlex.split(self.command)]
            self._run(arguments, cancelled)
            outputs = self._outputs(out_dir, folder, cancelled)
            if len(outputs) != len(clip):
                raise RuntimeError(f"the command wrote {len(outputs)} pictures for a clip of "
                                   f"{len(clip)}")
            return [area_resize(read_png(path), target_width, target_height)
                    for path in outputs]

    def _outputs(self, out_dir, folder, cancelled):
        pictures = sorted(glob.glob(os.path.join(out_dir, "**", "*.png"), recursive=True))
        if pictures:
            return pictures
        videos = sorted(glob.glob(os.path.join(out_dir, "**", "*.mp4"), recursive=True))
        if not videos:
            raise RuntimeError("the command wrote no PNG pictures and no video")
        frames = os.path.join(folder, "frames")
        os.makedirs(frames)
        self._run([self.ffmpeg, "-loglevel", "error", "-y", "-i", videos[0],
                   os.path.join(frames, "frame_%05d.png")], cancelled)
        return sorted(glob.glob(os.path.join(frames, "*.png")))

    @staticmethod
    def _run(arguments, cancelled):
        process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        while True:
            try:
                output, _ = process.communicate(timeout=0.5)
                break
            except subprocess.TimeoutExpired:
                if cancelled():
                    process.kill()
                    process.communicate()
                    raise Cancelled()
        if process.returncode != 0:
            tail = output.decode(errors="replace")[-2000:]
            raise RuntimeError(f"{arguments[0]} failed ({process.returncode}): {tail}")


# ---- Jobs ------------------------------------------------------------------------------------


class Job:
    def __init__(self, request):
        self.id = uuid.uuid4().hex
        self.request = request
        self.state = "queued"
        self.done = 0
        self.total = sum(len(clip) for clip in request["clips"])
        self.error = ""
        self.results = []
        self.cancel = threading.Event()

    def describe(self):
        answer = {"job": self.id, "state": self.state, "done": self.done, "total": self.total}
        if self.error:
            answer["error"] = self.error
        if self.state == "done":
            answer["results"] = self.results
        return answer


class Service:
    def __init__(self, backend, max_frames, token, max_kept=8):
        self.backend = backend
        self.max_frames = max_frames
        self.token = token
        self.max_kept = max_kept
        self.jobs = {}
        self.order = []
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.stopping = False
        self.worker = threading.Thread(target=self._work, daemon=True)
        self.worker.start()

    def info(self):
        with self.lock:
            busy = any(job.state in ("queued", "running") for job in self.jobs.values())
        return {"service": SERVICE, "protocol": PROTOCOL, "backend": self.backend.name,
                "models": self.backend.models, "maxFrames": self.max_frames, "busy": busy}

    def submit(self, body):
        request = parse_request(body, self.backend.models, self.max_frames)
        job = Job(request)
        with self.lock:
            self.jobs[job.id] = job
            self.order.append(job.id)
            # Finished jobs are kept a while for the plugin to fetch, the oldest dropped.
            while len(self.order) > self.max_kept:
                oldest = self.jobs.get(self.order[0])
                if oldest and oldest.state in ("queued", "running"):
                    break
                self.jobs.pop(self.order.pop(0), None)
            self.wake.notify()
        return job

    def job(self, job_id):
        with self.lock:
            return self.jobs.get(job_id)

    def cancel(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if not job:
                return None
            job.cancel.set()
            if job.state == "queued":
                job.state = "cancelled"
            return job

    def stop(self):
        with self.lock:
            self.stopping = True
            self.wake.notify()

    def _next(self):
        with self.lock:
            while not self.stopping:
                for job_id in self.order:
                    job = self.jobs.get(job_id)
                    if job and job.state == "queued":
                        job.state = "running"
                        return job
                self.wake.wait()
            return None

    def _work(self):
        while True:
            job = self._next()
            if job is None:
                return
            try:
                request = job.request
                results = []
                for clip, ids in zip(request["clips"], request["ids"]):
                    pictures = self.backend.upscale(clip, request["scale"], request["model"],
                                                    request["seed"], job.cancel.is_set)
                    for picture_id, source, result in zip(ids, clip, pictures):
                        width, height, rgb = result
                        if (width, height) != (source[0] * request["scale"],
                                               source[1] * request["scale"]):
                            raise RuntimeError(f"the backend's result for {picture_id} has "
                                               f"the size {width}x{height}")
                        results.append({"id": picture_id, "width": width, "height": height,
                                        "rgb": base64.b64encode(bytes(rgb)).decode("ascii")})
                    with self.lock:
                        job.done += len(clip)
                with self.lock:
                    job.results = results
                    job.state = "cancelled" if job.cancel.is_set() else "done"
            except Cancelled:
                with self.lock:
                    job.state = "cancelled"
            except Exception as error:  # pylint: disable=broad-except
                with self.lock:
                    job.state = "failed"
                    job.error = str(error)


def parse_request(body, models, max_frames):
    try:
        request = json.loads(body)
    except ValueError as error:
        raise RequestError(f"the request is not JSON: {error}") from error
    if not isinstance(request, dict):
        raise RequestError("the request is not a JSON object")
    if request.get("protocol", PROTOCOL) != PROTOCOL:
        raise RequestError(f"the service speaks protocol {PROTOCOL}")
    model = request.get("model") or models[0]
    if model not in models:
        raise RequestError(f"no model {model}; the service has {', '.join(models)}")
    scale = request.get("scale", 2)
    if not isinstance(scale, int) or not 1 <= scale <= 8:
        raise RequestError("the scale is a whole number from 1 to 8")
    seed = request.get("seed", 42)
    if not isinstance(seed, int):
        raise RequestError("the seed is a whole number")
    sequences = request.get("sequences")
    if not isinstance(sequences, list) or not sequences:
        raise RequestError("the request has no sequences")
    clips, ids, count = [], [], 0
    for sequence in sequences:
        frames = sequence.get("frames") if isinstance(sequence, dict) else None
        if not isinstance(frames, list) or not frames:
            raise RequestError("a sequence has no frames")
        clip, clip_ids = [], []
        for frame in frames:
            try:
                width, height = int(frame["width"]), int(frame["height"])
                rgb = base64.b64decode(frame["rgb"], validate=True)
                picture_id = str(frame["id"])
            except (KeyError, TypeError, ValueError) as error:
                raise RequestError(f"a frame is not as expected: {error}") from error
            if not (0 < width <= 1024 and 0 < height <= 1024) or len(rgb) != width * height * 3:
                raise RequestError(f"frame {picture_id} has not its size in RGB")
            if clip and (width, height) != (clip[0][0], clip[0][1]):
                raise RequestError("the frames of a sequence have different sizes")
            clip.append((width, height, bytearray(rgb)))
            clip_ids.append(picture_id)
        clips.append(clip)
        ids.append(clip_ids)
        count += len(clip)
    if max_frames and count > max_frames:
        raise RequestError(f"a job takes at most {max_frames} frames")
    return {"model": model, "scale": scale, "seed": seed, "clips": clips, "ids": ids}


# ---- HTTP ------------------------------------------------------------------------------------


def make_handler(service):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):  # pylint: disable=redefined-builtin
            if getattr(self.server, "verbose", False):
                super().log_message(format, *args)

        def _answer(self, status, body):
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _allowed(self):
            if not service.token:
                return True
            expected = "Bearer " + service.token
            given = self.headers.get("Authorization", "")
            if hmac.compare_digest(given.encode(), expected.encode()):
                return True
            self._answer(401, {"error": "a token is needed"})
            return False

        def _job_id(self):
            parts = self.path.rstrip("/").split("/")
            return parts[3] if len(parts) == 4 and parts[1:3] == ["v1", "jobs"] else None

        def do_GET(self):  # pylint: disable=invalid-name
            if not self._allowed():
                return
            if self.path == "/v1/info":
                self._answer(200, service.info())
                return
            job_id = self._job_id()
            job = service.job(job_id) if job_id else None
            if job is None:
                self._answer(404, {"error": "no such job"})
                return
            with service.lock:
                answer = job.describe()
            self._answer(200, answer)

        def do_POST(self):  # pylint: disable=invalid-name
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            if not self._allowed():
                return
            if self.path.rstrip("/") != "/v1/jobs":
                self._answer(404, {"error": "no such endpoint"})
                return
            try:
                job = service.submit(body)
            except RequestError as error:
                self._answer(400, {"error": str(error)})
                return
            self._answer(202, {"job": job.id})

        def do_DELETE(self):  # pylint: disable=invalid-name
            if not self._allowed():
                return
            job_id = self._job_id()
            job = service.cancel(job_id) if job_id else None
            if job is None:
                self._answer(404, {"error": "no such job"})
                return
            self._answer(200, {"job": job.id, "state": job.state})

    return Handler


class Server(ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer.server_bind looks up the host's full name, which can take half a minute
        # without a name server answering; the name is used for nothing here.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def make_server(service, host, port, verbose=False):
    server = Server((host, port), make_handler(service))
    server.daemon_threads = True
    server.verbose = verbose
    return server


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to listen on (0.0.0.0 for other computers; then set "
                             "--token)")
    parser.add_argument("--port", type=int, default=8976)
    parser.add_argument("--token", default=os.environ.get("SEEDVR_SERVICE_TOKEN", ""),
                        help="bearer token the plugin must send (or SEEDVR_SERVICE_TOKEN)")
    parser.add_argument("--backend", choices=["stub", "command"], default="stub")
    parser.add_argument("--command", default="",
                        help="the upscaler command for the command backend, with {input}, "
                             "{output}, {width}, {height}, {seed}, {model}, {count}, {scale}")
    parser.add_argument("--models", default="3b,7b", help="the models the plugin may ask for")
    parser.add_argument("--prescale", type=int, default=4,
                        help="nearest enlargement before the command (command backend)")
    parser.add_argument("--input-format", choices=["png", "mp4"], default="png")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--work-dir", default=None, help="folder for the clips' files")
    parser.add_argument("--max-frames", type=int, default=64,
                        help="the most frames one job takes")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    models = [model.strip() for model in args.models.split(",") if model.strip()]
    if args.backend == "command":
        if not args.command:
            parser.error("the command backend needs --command")
        backend = CommandBackend(models, args.command, args.prescale, args.input_format,
                                 args.ffmpeg, args.work_dir)
        if args.input_format == "mp4" and not shutil.which(args.ffmpeg):
            parser.error(f"--input-format mp4 needs {args.ffmpeg}")
    else:
        backend = StubBackend(models)
    if args.host not in ("127.0.0.1", "localhost", "::1") and not args.token:
        print("Warning: listening for other computers without --token", file=sys.stderr)

    service = Service(backend, args.max_frames, args.token)
    server = make_server(service, args.host, args.port, args.verbose)
    print(f"{SERVICE}: {backend.name} backend on http://{args.host}:{server.server_address[1]}",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()
        server.server_close()


if __name__ == "__main__":
    main()
