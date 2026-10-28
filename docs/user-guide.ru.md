# GuVi

![GuVi](logo3.png)

GuVi is a Windows screen recorder. It captures a monitor or a window with Windows.Graphics.Capture, keeps the frame on the GPU, and encodes H.264 or HEVC with NVENC, AMD AMF, or Intel Quick Sync. Audio is WASAPI to AAC. The file is constant-frame-rate MP4. A webcam is composited on the GPU and never waits on the game frame.

The app you run is `guvi.exe`.

## Features

- Record a monitor or a window, including a window that is minimized or not visible yet
- Constant FPS, with encoding on its own queue so the capture loop stays smooth
- Pause that holds the picture and freezes the on-screen timer
- Instant replay, screenshots, and an optional recording HUD
- RTMP / RTMPS streaming and a few image widgets on the stream only
- Interface in Russian, Ukrainian, English, and Polish (follows Windows, or pick a language from the gear)
- Update check against the public GitHub releases API for `gubke/guvi` (no token)

Recordings are named `guvi_YYYYMMDD_N.mp4`. `N` starts at 1 each day.

A take is written as fragmented MP4, with a sidecar `guvi_YYYYMMDD_N.mp4.guvi` (path, start time, expected name). If power is lost, the next launch repairs the file in place, keeps it under that name, and tells you the video was restored.

## Build

Requirements: Windows 10 or 11, Visual Studio 2022 Build Tools (x64), CMake, and Ninja.

From an x64 developer prompt:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset release
cmake --build --preset release --target recorder_gui
```

The GUI target is still named `recorder_gui` in CMake. The executable it links is `build\guvi.exe`. Language files are copied to `build\lang\`.

`recorder.exe` is the console tool for the same capture core. It is not the app users launch.

## Change the app

- Version, window title, and the GitHub repo name used for update checks: `src/common/version.h`
- Interface strings: `lang/en.json`, `lang/uk.json`, `lang/pl.json` (keys are the Russian text)
- Capture and encode loop: `src/app/recorder_engine.cpp`
- Window UI: `src/gui/gui_main.cpp`
- Icon: `logo3.ico` (embedded by `guvi.rc`). `logo3.png` is the logo in this README.

Do not put access tokens in the tree. The update check is an anonymous GET to `https://api.github.com/repos/gubke/guvi/releases/latest`.

## About

Gubke. Created 28.10.26. Co-created with Cursor.

- YouTube: https://www.youtube.com/@gubke
- Telegram: @negubke
- Mail: gubke.info@gmail.com
- Steam: https://steamcommunity.com/id/gubke/
