# ColorizingDMD SeedVR Upscaler

A plugin for the [ColorizingDMD](https://github.com/PPUC/ColorizingDMD) editor that upscales
colorized SD pictures (128x32) to the HD plane (256x64) with
[SeedVR2](https://github.com/ByteDance-Seed/SeedVR). It works on frames, backgrounds and
sprites. SeedVR2 needs Linux, an NVIDIA GPU with CUDA, PyTorch and several GB of models, so it
runs as a service of its own, on the same computer or on another one. The plugin sends the
pictures to that service, brings the results back to RGB565 exactly, and proposes them in the
editor's review window. One Undo takes back what was applied.

Needs the editor with editor API version 3.

## Parts

- `src/`: the plugin (C++, the editor API `plugin-api/ColorizingDMDPlugin.h` on the VPX
  plugin SDK).
  - `Pictures`: what is sent, and how a result becomes a picture (RGB565, the color modes, the
    write mask).
  - `ServiceClient`: the HTTP protocol.
  - `Upscale`: gathering from the editor, clips and jobs.
  - `Plugin.cpp`: the entry points, the menu and the worker thread.
- `service/seedvr_service.py`: the service, Python standard library only. It has a stub
  backend (nearest upscaling, for tests and for trying the plugin without a GPU) and a command
  backend that runs SeedVR2.
- `tests/`: the plugin's tests (no editor needed); `service/test_service.py`: the service's.
- `third-party/`: cpp-httplib 0.18.3 and nlohmann/json 3.11.3.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCOLORIZINGDMD_DIR=/path/to/ColorizingDMD \
        -DVPINBALL_DIR=/path/to/vpinball \
        -DCOLORIZINGDMD_PLUGIN_DIR=/path/to/editor/build/vpx-plugins
    cmake --build build
    ctest --test-dir build --output-on-failure

The build puts the plugin's folder in `build/SeedVR`: `plugin.cfg` and the library. With
`COLORIZINGDMD_PLUGIN_DIR` set, it is also copied into a development editor's plugin folder. For
a packaged editor, copy `SeedVR` into its plugin folder: `plugins` beside the program on Windows
and Linux, `ColorizingDMD.app/Contents/PlugIns/vpx` on macOS.

## Use

1. Start the service (see below). To try the plugin without a GPU:
   `python3 service/seedvr_service.py --backend stub`.
2. In the editor, check `SeedVR Upscaler` under `Settings -> Plugins`.
3. `Plugins -> SeedVR: Settings...` sets the service's address (default
   `http://127.0.0.1:8976`), its token, the model, the seed and how the results are made:
   - Colors:
     - `Colors of the pixels around` (the default): each HD pixel takes the nearest of the
       colors of the 3x3 SD pixels around the one under it. This keeps edges crisp and invents
       no color.
     - `Colors of the picture`: the nearest of all the colors the picture uses.
     - `Any color`: the result rounded to RGB565.
   - `Send frames with their background`: where a frame's background mask shows the background,
     the upscaler sees the background, so that no edge appears there. The result is written only
     where the frame shows its own pixels; the background mask decides. Without this option,
     those pixels take the nearest frame color before they are sent.
   - `Leave dynamic zones as they are`.
   - `Frames per clip`: consecutive selected frames are upscaled together as a video clip, which
     keeps the result steady from one frame to the next.
4. Select frames and choose `Plugins -> SeedVR: Upscale Selected Frames...`. The other entries
   upscale the backgrounds or the sprites of the selected frames, or the background or sprite
   chosen in their panel. The status bar shows the progress; `SeedVR: Cancel Upscaling` stops
   it.
5. The review window shows each picture now and as proposed. Uncheck what should stay as it is,
   then `Apply`. A frame, background or sprite without HD gets it first, as `Create HD` makes it.

The project must have an HD plane of twice the SD size. A sprite is upscaled within the part
its SD shape covers, and the result is written where its HD shape covers. When the sprite has no
HD yet, that is the SD shape doubled.

## The service

    python3 service/seedvr_service.py --backend stub
    python3 service/seedvr_service.py --backend command --command '<upscaler command>'

The service listens on 127.0.0.1:8976. For another computer, the safest way is an SSH tunnel
(`ssh -L 8976:127.0.0.1:8976 gpu-box`). Otherwise use `--host 0.0.0.0 --token <secret>`, with the
same token in the plugin's settings. The plugin speaks plain HTTP only.

The command backend runs the upscaler command once per clip:

1. It enlarges the clip's pictures by `--prescale` (default 4, nearest), as SeedVR2 works on
   larger pictures than 128x32.
2. It writes them as `frame_00000.png`, ... into a folder, or as a lossless video with
   `--input-format mp4` (needs ffmpeg).
3. It runs the command, with these placeholders filled in:
   - `{input}`: the folder or the video.
   - `{output}`: an empty folder for the results.
   - `{width}` and `{height}`: the size to upscale to.
   - `{seed}`, `{model}`, `{count}` (pictures in the clip) and `{scale}`.
4. It reads what the command wrote into the output folder: PNG files in name order, or one
   video, which ffmpeg decodes.
5. It scales the results down to exactly twice the original size, each pixel the average of the
   area it covers.

The plugin then brings each result to RGB565.

With SeedVR's own repository, on a Linux machine with CUDA, set up as its README describes, a
command along these lines runs the 3B model:

    python3 service/seedvr_service.py --backend command --input-format mp4 --command \
      'torchrun --nproc-per-node=1 /opt/SeedVR/projects/inference_seedvr2_3b.py
       --video_path {input} --output_dir {output} --seed {seed}
       --res_h {height} --res_w {width} --sp_size 1'

Check the script's options against the SeedVR version you have; they are SeedVR's and may
change. Other front ends of SeedVR2 that can be run from a command line work the same way. The
command loads the model for every clip; that is slow, but it keeps the service independent of
SeedVR's code.

### Protocol 1

All requests and answers are JSON. With a token, every request carries
`Authorization: Bearer <token>`.

- `GET /v1/info` returns
  `{"service": "colorizingdmd-seedvr", "protocol": 1, "backend": "stub" | "command",
  "models": [...], "maxFrames": n, "busy": bool}`.
- `POST /v1/jobs` takes
  `{"protocol": 1, "model": "3b", "seed": 42, "scale": 2, "sequences": [{"frames": [{"id":
  "f12", "width": 128, "height": 32, "rgb": "<base64 RGB, 3 bytes a pixel>", "alpha":
  "<base64, 1 byte a pixel, optional>"}, ...]}, ...]}`. It answers 202 with `{"job": "<id>"}`,
  or 400 with `{"error": "..."}`.
  - Each sequence is one clip; its pictures have one size.
  - IDs are the plugin's: `f<frame>`, `b<background>`, `s<sprite>`.
  - A job holds at most `maxFrames` pictures.
- `GET /v1/jobs/<id>` returns
  `{"job": id, "state": "queued" | "running" | "done" | "failed" | "cancelled", "done": k,
  "total": n, "error": "..."}`. When the job is done, the answer also holds
  `"results": [{"id": "f12", "width": 256, "height": 64, "rgb": "<base64>"}]`, each exactly
  `scale` times its picture.
- `DELETE /v1/jobs/<id>` cancels the job.

Jobs run one after the other. Finished jobs are kept for the plugin to fetch; the oldest are
dropped after eight.

## Licence

GPLv3 or later (`LICENSE`), as the editor. SeedVR's code and models are Apache 2.0; the service
runs them as a separate program and contains none of them.
