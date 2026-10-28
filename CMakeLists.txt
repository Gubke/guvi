cmake_minimum_required(VERSION 3.21)
project(screen_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "Нужен MSVC (Visual Studio 2022) — C++/WinRT и Windows SDK")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Собирайте x64: nvEncodeAPI64.dll и WGC используются в 64-битном процессе")
endif()

# По умолчанию — вложенные FFmpeg nv-codec-headers (third_party/nv-codec-headers, тег n13.0.19.1).
# Переопределение: -DNVENC_INCLUDE_DIR=<папка с nvEncodeAPI.h> (например Video Codec SDK/Interface).
set(NVENC_BUNDLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers/include/ffnvcodec")
set(NVENC_INCLUDE_DIR "${NVENC_BUNDLED_DIR}" CACHE PATH "Папка, где лежит nvEncodeAPI.h")

if(NOT EXISTS "${NVENC_INCLUDE_DIR}/nvEncodeAPI.h")
    message(FATAL_ERROR
        "Не найден ${NVENC_INCLUDE_DIR}/nvEncodeAPI.h\n"
        "Верните third_party/nv-codec-headers (см. README.md) или укажите -DNVENC_INCLUDE_DIR=<папка>")
endif()
message(STATUS "NVENC header: ${NVENC_INCLUDE_DIR}/nvEncodeAPI.h")

set(RECORDER_DEFINES UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
set(RECORDER_OPTIONS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /bigobj)

# ---- Ядро: захват, кодирование, аудио, мукс, движок записи ----
add_library(recorder_core STATIC
    src/common/args.cpp
    src/common/d3d_device.cpp
    src/common/log.cpp
    src/common/i18n.cpp
    src/capture/wgc_capture.cpp
    src/capture/scaler.cpp
    src/capture/webcam_source.cpp
    src/capture/webcam_compositor.cpp
    src/capture/overlay_compositor.cpp
    src/encode/video_encoder.cpp
    src/encode/nvenc_encoder.cpp
    src/encode/amf_encoder.cpp
    src/encode/mf_encoder.cpp
    src/audio/wasapi_source.cpp
    src/audio/aac_encoder.cpp
    src/audio/audio_engine.cpp
    src/mux/muxer.cpp
    src/app/hotkeys.cpp
    src/app/recording_session.cpp
    src/app/replay_buffer.cpp
    src/app/screenshot.cpp
    src/app/recorder_engine.cpp
    src/app/overlay_hud.cpp
    src/stream/net_transport.cpp
    src/stream/rtmp_publisher.cpp
)
target_include_directories(recorder_core PUBLIC src)
target_include_directories(recorder_core SYSTEM PUBLIC "${NVENC_INCLUDE_DIR}")
# AMD AMF: только заголовки (third_party/amf, MIT), amfrt64.dll приходит с драйвером.
target_include_directories(recorder_core SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf/include")
target_compile_definitions(recorder_core PUBLIC ${RECORDER_DEFINES})
target_compile_options(recorder_core PRIVATE ${RECORDER_OPTIONS})
target_link_libraries(recorder_core PUBLIC
    d3d11 dxgi dwmapi windowsapp ole32
    mfplat mfreadwrite mfuuid
    windowscodecs winmm gdiplus d3dcompiler
    mf strmiids oleaut32
    ws2_32 secur32 crypt32
)

# ---- CLI ----
add_executable(recorder src/main.cpp)
target_compile_options(recorder PRIVATE ${RECORDER_OPTIONS})
target_link_libraries(recorder PRIVATE recorder_core)

# ---- Dear ImGui (third_party/imgui, v1.92.9) ----
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui SYSTEM PUBLIC "${IMGUI_DIR}")
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE /utf-8 /W0)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)

# ---- GUI (WIN32 subsystem) ----
add_executable(recorder_gui WIN32
    src/gui/gui_main.cpp
    src/gui/settings.cpp
    src/gui/library.cpp
    src/gui/url_image.cpp
    guvi.rc
)
set_target_properties(recorder_gui PROPERTIES OUTPUT_NAME guvi)
target_compile_options(recorder_gui PRIVATE ${RECORDER_OPTIONS})
target_link_libraries(recorder_gui PRIVATE recorder_core imgui shell32 ole32 uuid winhttp windowscodecs gdiplus)
add_custom_command(TARGET recorder_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lang" "$<TARGET_FILE_DIR:recorder_gui>/lang")

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT recorder_gui)
