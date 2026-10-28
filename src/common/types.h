#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

enum class Codec { H264, HEVC };

// Время в единицах 100 нс (как в Media Foundation).
inline constexpr int64_t kTimescale = 10'000'000;

// Пакет прямо из кодировщика (данные принадлежат кодировщику до возврата из колбэка).
struct EncodedPacket {
    std::span<const uint8_t> data;  // Annex B
    bool keyframe = false;
    int64_t pts = 0;
};

enum class StreamKind { Video, Audio };

// Владеющий пакет: общий для записи, replay-буфера и фоновых сохранений.
struct Packet {
    StreamKind stream = StreamKind::Video;
    int64_t pts = 0;       // 100 нс от старта MediaClock
    int64_t duration = 0;  // 100 нс
    bool keyframe = false;
    std::vector<uint8_t> data;
};
using PacketPtr = std::shared_ptr<const Packet>;

struct VideoStreamInfo {
    Codec codec = Codec::H264;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 20000;
    uint32_t keyintSeconds = 2;
    bool lowLatency = true;  // пресет: true — низкая задержка и минимум нагрузки на GPU, false — качество
};

struct AudioStreamInfo {
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bitrateKbps = 192;
    std::vector<uint8_t> aacUserData;  // MF_MT_USER_DATA: хвост HEAACWAVEINFO + AudioSpecificConfig
};

struct StreamSetup {
    VideoStreamInfo video;
    std::vector<uint8_t> videoSequenceHeader;
    std::optional<AudioStreamInfo> audio;
};
