# GuVi

![GuVi](logo3.png)

**A free Windows screen recorder and streamer.** Capture a game, one window, or the whole display, then keep the file or send it out live.

Free Windows screen recorder · game capture · window capture · RTMP streaming · instant replay · webcam overlay · draggable widgets · NVENC · open source

GuVi is new, free, and open source. It is a small Windows app for recording a match, a program window, or the full screen, and for streaming that picture over RTMP. You can talk on the mic, put a webcam over the video, and drop a few draggable widgets onto the stream. Encoding runs on NVIDIA (NVENC), AMD, or Intel hardware when the PC has it.

If the power goes out mid-take, the recording is not thrown away. GuVi writes the file so the next launch can restore it and tell you the video is back. Files are named `guvi_YYYYMMDD_N.mp4`.

## Features

- Game capture, window capture, or the whole display
- RTMP and RTMPS streaming
- Instant replay and screenshots
- Webcam overlay and microphone, mixed with system audio
- Draggable widgets on the stream
- NVENC, AMD, or Intel hardware encoding (H.264 or HEVC)
- Pause that holds the picture and stops the on-screen timer
- Crash-safe recordings that survive a power loss
- Interface in English, Ukrainian, Polish, and Russian

## Download

GuVi 1.0.1 is on the [release page](https://github.com/Gubke/guvi/releases/tag/v1.0.1).

[Download the portable zip](https://github.com/Gubke/guvi/releases/download/v1.0.1/GuVi-1.0.1-portable.zip) and run `guvi.exe`.

[Download GuVi-Setup.exe](https://github.com/Gubke/guvi/releases/download/v1.0.1/GuVi-Setup.exe) if you want an installer.

## Languages

English, Ukrainian, Polish, Russian. GuVi follows Windows, or you pick one of these in the gear menu.

## Author

Gubke. Created 28.10.26. Co-created with Cursor.

- YouTube: https://www.youtube.com/@gubke
- Telegram: [@negubke](https://t.me/negubke)
- Mail: [gubke.info@gmail.com](mailto:gubke.info@gmail.com)
- Steam: https://steamcommunity.com/id/gubke/

## Build

Developers need Windows 10 or 11, Visual Studio 2022 Build Tools (x64), CMake, and Ninja.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset release
cmake --build --preset release --target recorder_gui
```

The CMake target is `recorder_gui`. The program it builds is `build\guvi.exe`. The icon is `logo3.ico`.
