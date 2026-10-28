# GuVi

![GuVi](logo3.png)

GuVi is a free, open-source, unique Windows recorder and streamer. It captures a game, a window, or the whole display, streams over RTMP, and keeps an instant replay. A webcam and a mic sit in the take, and draggable widgets go on the stream. Encoding uses NVIDIA, AMD, or Intel hardware. The interface is English, Ukrainian, Polish, or Russian.

Recordings are crash-safe. A take is written as a fragmented MP4, so a power loss does not throw it away. The next launch repairs the file, keeps it as `guvi_YYYYMMDD_N.mp4`, and tells you the video was restored.

## Download

[GuVi 1.0.0](https://github.com/Gubke/guvi/releases/tag/v1.0.0): portable zip and Windows installer.

- Portable (unzip and run `guvi.exe`): https://github.com/Gubke/guvi/releases/download/v1.0.0/GuVi-1.0.0-portable.zip
- Installer: https://github.com/Gubke/guvi/releases/download/v1.0.0/GuVi-Setup.exe

## What it records

- A monitor, or a window, including one that is minimized or not on screen yet
- Microphone and system audio, mixed to AAC
- A webcam composited on the GPU, so it does not wait on the game frame
- H.264 or HEVC at a constant frame rate
- Pause that holds the picture and freezes the on-screen timer
- Instant replay, screenshots, and an optional recording HUD
- RTMP / RTMPS streaming

Recordings are named `guvi_YYYYMMDD_N.mp4`. `N` starts at 1 each day.

## Author

Gubke. Created 28.10.26. Co-created with Cursor.

- YouTube: https://www.youtube.com/@gubke
- Telegram: https://t.me/negubke (@negubke)
- Mail: gubke.info@gmail.com
- Steam: https://steamcommunity.com/id/gubke/

## Build

Windows 10 or 11, Visual Studio 2022 Build Tools (x64), CMake, and Ninja.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset release
cmake --build --preset release --target recorder_gui
```

The CMake target is `recorder_gui`. The executable is `build\guvi.exe`. The app icon is `logo3.ico`. `logo3.png` is the picture at the top of this page.

The update check is an anonymous GET to `https://api.github.com/repos/gubke/guvi/releases/latest`. Do not put access tokens in this tree.
