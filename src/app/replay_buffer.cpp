#include "app/replay_buffer.h"

void ReplayBuffer::Push(const PacketPtr& p)
{
    if (p->stream == StreamKind::Video) {
        if (video_.empty() && !p->keyframe)
            return;
        video_.push_back(p);
    } else {
        audio_.push_back(p);
    }
    bytes_ += p->data.size();
    Trim();
}

void ReplayBuffer::Trim()
{
    auto popVideo = [&] {
        bytes_ -= video_.front()->data.size();
        video_.pop_front();
    };
    auto popAudio = [&] {
        bytes_ -= audio_.front()->data.size();
        audio_.pop_front();
    };

    if (!video_.empty()) {
        const int64_t cutoff = video_.back()->pts - window_;
        // Ищем второй IDR: если и он старше окна, первый GOP целиком лишний.
        for (;;) {
            size_t nextKey = 1;
            while (nextKey < video_.size() && !video_[nextKey]->keyframe)
                ++nextKey;
            if (nextKey >= video_.size() || video_[nextKey]->pts > cutoff)
                break;
            for (size_t i = 0; i < nextKey; ++i)
                popVideo();
        }
    }

    const int64_t audioFloor = !video_.empty() ? video_.front()->pts
                             : !audio_.empty() ? audio_.back()->pts - window_
                                               : 0;
    while (!audio_.empty() && audio_.front()->pts < audioFloor)
        popAudio();
}

std::vector<PacketPtr> ReplayBuffer::Snapshot(int64_t cutoff) const
{
    std::vector<PacketPtr> out;
    if (video_.empty())
        return out;

    size_t first = 0;
    for (size_t i = 0; i < video_.size(); ++i) {
        if (video_[i]->pts > cutoff - window_)
            break;
        if (video_[i]->keyframe)
            first = i;
    }
    const int64_t startPts = video_[first]->pts;

    size_t v = first;
    size_t a = 0;
    while (a < audio_.size() && audio_[a]->pts < startPts)
        ++a;

    // Слияние по PTS, чтобы потоки в MP4 шли перемежаясь.
    for (;;) {
        const bool hasV = v < video_.size() && video_[v]->pts < cutoff;
        const bool hasA = a < audio_.size() && audio_[a]->pts < cutoff;
        if (!hasV && !hasA)
            break;
        if (hasV && (!hasA || video_[v]->pts <= audio_[a]->pts))
            out.push_back(video_[v++]);
        else
            out.push_back(audio_[a++]);
    }
    return out;
}
