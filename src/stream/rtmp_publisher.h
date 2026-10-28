#pragma once

#include "common/types.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class StreamState { Off, Connecting, Live, Reconnecting, Failed };

struct StreamTarget {
    std::string url;  // rtmp://host[:port]/app[/...] или rtmps://...
    std::string key;  // имя потока; никогда не пишется в журнал
    bool reconnect = true;
};

// Публикация в RTMP/RTMPS в своём потоке: подключение, FLV-теги H.264 / HEVC (Enhanced RTMP) + AAC,
// переподключение. Push* только кладут в очередь и не ждут сеть; при заторе очередь сбрасывается
// до следующего ключевого кадра, который запрашивается у кодировщика (TakeKeyframeRequest).
class RtmpPublisher {
public:
    RtmpPublisher(StreamTarget target, VideoStreamInfo video, std::vector<uint8_t> videoSequenceHeader,
                  std::optional<AudioStreamInfo> audio, int64_t basePts);
    ~RtmpPublisher();

    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    void PushVideo(const EncodedPacket& p);  // поток кодирования
    void PushAudio(const PacketPtr& p);      // поток движка
    bool TakeKeyframeRequest() { return keyRequest_.exchange(false); }

    StreamState State() const { return state_.load(); }
    std::string Error() const;
    double Kbps() const { return kbps_.load(); }
    uint64_t Drops() const
    {
        std::lock_guard lock(mtx_);
        return drops_;
    }

private:
    struct Item {
        bool video = false;
        bool keyframe = false;
        int64_t pts = 0;
        std::vector<uint8_t> data;
    };

    void ThreadMain();
    void Session();
    void SetError(const std::string& e);

    StreamTarget target_;
    VideoStreamInfo video_;
    std::vector<uint8_t> videoHeader_;
    std::optional<AudioStreamInfo> audio_;
    int64_t basePts_ = 0;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    size_t queuedBytes_ = 0;
    size_t maxQueuedBytes_ = 0;
    bool accepting_ = false;  // пакеты принимаются только в состоянии Live
    bool flushed_ = false;    // очередь сброшена из-за затора — ждать ключевой кадр
    bool finished_ = false;   // поток публикации завершился
    uint64_t drops_ = 0;      // сбросов очереди из-за затора сети
    std::string error_;
    std::atomic<bool> reachedLive_{ false };

    std::atomic<bool> stop_{ false };
    std::atomic<bool> keyRequest_{ false };
    std::atomic<StreamState> state_{ StreamState::Connecting };
    std::atomic<double> kbps_{ 0.0 };
    std::atomic<void*> transport_{ nullptr };  // NetTransport* активного соединения — для Abort()
    std::thread thread_;
};
