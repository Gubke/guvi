#include "stream/rtmp_publisher.h"
#include "stream/net_transport.h"

#include "common/clock.h"
#include "common/log.h"
#include "common/version.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

// ---------- Адрес ----------
struct RtmpUrl {
    bool tls = false;
    std::string host;
    uint16_t port = 1935;
    std::string app;
    std::string tcUrl;
};

RtmpUrl ParseUrl(const std::string& url)
{
    RtmpUrl u;
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string rest;
    std::string scheme;
    if (lower.rfind("rtmps://", 0) == 0) {
        u.tls = true;
        u.port = 443;
        scheme = "rtmps";
        rest = url.substr(8);
    } else if (lower.rfind("rtmp://", 0) == 0) {
        scheme = "rtmp";
        rest = url.substr(7);
    } else {
        throw std::runtime_error("адрес сервера должен начинаться с rtmp:// или rtmps://");
    }
    const size_t slash = rest.find('/');
    const std::string hostPort = rest.substr(0, slash);
    u.app = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    while (!u.app.empty() && u.app.back() == '/')
        u.app.pop_back();
    if (u.app.empty())
        throw std::runtime_error("в адресе нет имени приложения (…/app, например rtmp://live.twitch.tv/app)");

    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        u.host = hostPort.substr(0, colon);
        const int port = std::atoi(hostPort.c_str() + colon + 1);
        if (port <= 0 || port > 65535)
            throw std::runtime_error("неверный порт в адресе сервера");
        u.port = static_cast<uint16_t>(port);
    } else {
        u.host = hostPort;
    }
    if (u.host.empty())
        throw std::runtime_error("в адресе нет имени сервера");
    u.tcUrl = scheme + "://" + hostPort + "/" + u.app;
    return u;
}

std::string Redact(std::string s, const std::string& key)
{
    if (key.size() < 4)
        return s;
    for (size_t pos; (pos = s.find(key)) != std::string::npos;)
        s.replace(pos, key.size(), "***");
    return s;
}

// ---------- Байты ----------
void Be16(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void Be24(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 16));
    Be16(b, v);
}
void Be32(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 24));
    Be24(b, v);
}
uint32_t RdBe16(const uint8_t* p) { return (uint32_t{ p[0] } << 8) | p[1]; }
uint32_t RdBe24(const uint8_t* p) { return (uint32_t{ p[0] } << 16) | RdBe16(p + 1); }
uint32_t RdBe32(const uint8_t* p) { return (uint32_t{ p[0] } << 24) | RdBe24(p + 1); }

// ---------- AMF0 ----------
struct Amf {
    std::vector<uint8_t> b;
    void Num(double v)
    {
        b.push_back(0x00);
        const uint64_t u = std::bit_cast<uint64_t>(v);
        for (int i = 7; i >= 0; --i)
            b.push_back(static_cast<uint8_t>(u >> (i * 8)));
    }
    void Bool(bool v)
    {
        b.push_back(0x01);
        b.push_back(v ? 1 : 0);
    }
    void Key(std::string_view k)
    {
        Be16(b, static_cast<uint32_t>(k.size()));
        b.insert(b.end(), k.begin(), k.end());
    }
    void Str(std::string_view s)
    {
        b.push_back(0x02);
        Key(s);
    }
    void Null() { b.push_back(0x05); }
    void ObjBegin() { b.push_back(0x03); }
    void EcmaBegin(uint32_t count)
    {
        b.push_back(0x08);
        Be32(b, count);
    }
    void ObjEnd()
    {
        b.push_back(0);
        b.push_back(0);
        b.push_back(0x09);
    }
};

struct AmfValue {
    int type = 5;
    double num = 0;
    bool flag = false;
    std::string str;
    std::vector<std::pair<std::string, AmfValue>> props;

    const AmfValue* Get(std::string_view k) const
    {
        for (const auto& [name, v] : props)
            if (name == k)
                return &v;
        return nullptr;
    }
    std::string GetStr(std::string_view k) const
    {
        const AmfValue* v = Get(k);
        return v && v->type == 2 ? v->str : std::string();
    }
};

bool ReadAmf(const uint8_t*& p, const uint8_t* end, AmfValue& v, int depth = 0)
{
    if (p >= end || depth > 16)
        return false;
    v.type = *p++;
    auto need = [&](size_t n) { return static_cast<size_t>(end - p) >= n; };
    auto readProps = [&]() {
        for (;;) {
            if (!need(3))
                return false;
            const uint32_t len = RdBe16(p);
            if (len == 0 && p[2] == 0x09) {
                p += 3;
                return true;
            }
            p += 2;
            if (!need(len))
                return false;
            std::string name(reinterpret_cast<const char*>(p), len);
            p += len;
            AmfValue child;
            if (!ReadAmf(p, end, child, depth + 1))
                return false;
            v.props.emplace_back(std::move(name), std::move(child));
        }
    };
    switch (v.type) {
    case 0x00:
        if (!need(8))
            return false;
        {
            uint64_t u = 0;
            for (int i = 0; i < 8; ++i)
                u = (u << 8) | p[i];
            v.num = std::bit_cast<double>(u);
        }
        p += 8;
        return true;
    case 0x01:
        if (!need(1))
            return false;
        v.flag = *p++ != 0;
        return true;
    case 0x02: {
        if (!need(2))
            return false;
        const uint32_t len = RdBe16(p);
        p += 2;
        if (!need(len))
            return false;
        v.str.assign(reinterpret_cast<const char*>(p), len);
        p += len;
        return true;
    }
    case 0x0C: {
        if (!need(4))
            return false;
        const uint32_t len = RdBe32(p);
        p += 4;
        if (!need(len))
            return false;
        v.str.assign(reinterpret_cast<const char*>(p), len);
        v.type = 0x02;
        p += len;
        return true;
    }
    case 0x03:
        return readProps();
    case 0x08:
        if (!need(4))
            return false;
        p += 4;
        return readProps();
    case 0x0A: {
        if (!need(4))
            return false;
        const uint32_t n = RdBe32(p);
        p += 4;
        for (uint32_t i = 0; i < n; ++i) {
            AmfValue child;
            if (!ReadAmf(p, end, child, depth + 1))
                return false;
        }
        return true;
    }
    case 0x0B:
        if (!need(10))
            return false;
        p += 10;
        return true;
    case 0x05:
    case 0x06:
        return true;
    default:
        return false;
    }
}

std::vector<AmfValue> ParseCommand(const std::vector<uint8_t>& data, uint8_t type)
{
    std::vector<AmfValue> out;
    const uint8_t* p = data.data();
    const uint8_t* end = p + data.size();
    if (type == 17 && p < end)
        ++p;  // AMF3-команда: первый байт 0, дальше AMF0
    while (p < end) {
        AmfValue v;
        if (!ReadAmf(p, end, v))
            break;
        out.push_back(std::move(v));
    }
    return out;
}

// ---------- H.264 / HEVC → FLV ----------
std::vector<std::span<const uint8_t>> SplitNals(const uint8_t* d, size_t n)
{
    std::vector<std::span<const uint8_t>> out;
    size_t i = 0;
    size_t start = SIZE_MAX;
    while (i + 2 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t e = i;
                while (e > start && d[e - 1] == 0)
                    --e;
                if (e > start)
                    out.emplace_back(d + start, e - start);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n)
        out.emplace_back(d + start, n - start);
    return out;
}

int NalType(Codec c, std::span<const uint8_t> nal)
{
    if (nal.empty())
        return -1;
    return c == Codec::H264 ? (nal[0] & 0x1F) : ((nal[0] >> 1) & 0x3F);
}

bool IsAud(Codec c, int t)
{
    return c == Codec::H264 ? t == 9 : t == 35;
}

std::vector<uint8_t> Unescape(std::span<const uint8_t> nal)
{
    std::vector<uint8_t> out;
    int zeros = 0;
    for (const uint8_t b : nal) {
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> SequenceTag(Codec codec, const std::vector<uint8_t>& annexB)
{
    std::span<const uint8_t> vps, sps, pps;
    for (const auto nal : SplitNals(annexB.data(), annexB.size())) {
        const int t = NalType(codec, nal);
        if (codec == Codec::H264) {
            if (t == 7 && sps.empty())
                sps = nal;
            else if (t == 8 && pps.empty())
                pps = nal;
        } else {
            if (t == 32 && vps.empty())
                vps = nal;
            else if (t == 33 && sps.empty())
                sps = nal;
            else if (t == 34 && pps.empty())
                pps = nal;
        }
    }
    if (sps.size() < 4 || pps.empty() || (codec == Codec::HEVC && vps.empty()))
        throw std::runtime_error("кодировщик не отдал SPS/PPS для трансляции");

    std::vector<uint8_t> t;
    if (codec == Codec::H264) {
        t = { 0x17, 0x00, 0, 0, 0 };
        // AVCDecoderConfigurationRecord
        t.push_back(1);
        t.push_back(sps[1]);
        t.push_back(sps[2]);
        t.push_back(sps[3]);
        t.push_back(0xFF);  // lengthSizeMinusOne = 3
        t.push_back(0xE1);  // 1 SPS
        Be16(t, static_cast<uint32_t>(sps.size()));
        t.insert(t.end(), sps.begin(), sps.end());
        t.push_back(1);
        Be16(t, static_cast<uint32_t>(pps.size()));
        t.insert(t.end(), pps.begin(), pps.end());
        return t;
    }

    // Enhanced RTMP: ExHeader | FrameType=key | PacketType=SequenceStart, FourCC hvc1.
    t = { 0x80 | (1 << 4) | 0, 'h', 'v', 'c', '1' };
    // HEVCDecoderConfigurationRecord: profile_tier_level берётся из SPS (после 2 байт заголовка NAL).
    const std::vector<uint8_t> rbsp = Unescape(sps.subspan(2));
    if (rbsp.size() < 13)
        throw std::runtime_error("HEVC SPS слишком короткий");
    const uint8_t maxSubLayersMinus1 = (rbsp[0] >> 1) & 0x07;
    const uint8_t temporalIdNested = rbsp[0] & 0x01;
    t.push_back(1);
    t.insert(t.end(), rbsp.begin() + 1, rbsp.begin() + 13);  // profile/tier/idc, compat (4), constraint (6), level
    t.push_back(0xF0);
    t.push_back(0x00);                      // min_spatial_segmentation_idc
    t.push_back(0xFC);                      // parallelismType
    t.push_back(0xFD);                      // chroma 4:2:0
    t.push_back(0xF8);                      // bitDepthLuma 8
    t.push_back(0xF8);                      // bitDepthChroma 8
    Be16(t, 0);                             // avgFrameRate
    t.push_back(static_cast<uint8_t>(((maxSubLayersMinus1 + 1) << 3) | (temporalIdNested << 2) | 3));
    t.push_back(3);
    for (const auto& [type, nal] : { std::pair{ 32, vps }, std::pair{ 33, sps }, std::pair{ 34, pps } }) {
        t.push_back(static_cast<uint8_t>(0x80 | type));
        Be16(t, 1);
        Be16(t, static_cast<uint32_t>(nal.size()));
        t.insert(t.end(), nal.begin(), nal.end());
    }
    return t;
}

void VideoTag(Codec codec, bool key, const std::vector<uint8_t>& annexB, std::vector<uint8_t>& t)
{
    t.clear();
    if (codec == Codec::H264)
        t = { static_cast<uint8_t>(key ? 0x17 : 0x27), 0x01, 0, 0, 0 };
    else  // CodedFramesX: без поля composition time (B-кадров нет)
        t = { static_cast<uint8_t>(0x80 | ((key ? 1 : 2) << 4) | 3), 'h', 'v', 'c', '1' };
    for (const auto nal : SplitNals(annexB.data(), annexB.size())) {
        if (IsAud(codec, NalType(codec, nal)))
            continue;
        Be32(t, static_cast<uint32_t>(nal.size()));
        t.insert(t.end(), nal.begin(), nal.end());
    }
}

// ---------- RTMP ----------
class RtmpClient {
public:
    explicit RtmpClient(NetTransport& t, std::string key) : t_(t), key_(std::move(key)) {}

    void Connect(const RtmpUrl& u, bool hevc)
    {
        Handshake();
        {
            std::vector<uint8_t> p;
            Be32(p, 4096);
            Send(1, 2, 0, 0, p.data(), p.size());
            outChunk_ = 4096;
        }
        {
            Amf a;
            a.Str("connect");
            a.Num(1);
            a.ObjBegin();
            a.Key("app");
            a.Str(u.app);
            a.Key("type");
            a.Str("nonprivate");
            a.Key("flashVer");
            a.Str("FMLE/3.0 (compatible; " GUVI_NAME " " GUVI_VERSION ")");
            a.Key("tcUrl");
            a.Str(u.tcUrl);
            if (hevc) {
                a.Key("fourCcList");
                a.b.push_back(0x0A);
                Be32(a.b, 1);
                a.Str("hvc1");
            }
            a.ObjEnd();
            Command(0, a);
            WaitResult(1, "connect");
        }
        {
            Amf a;
            a.Str("releaseStream");
            a.Num(2);
            a.Null();
            a.Str(key_);
            Command(0, a);
        }
        {
            Amf a;
            a.Str("FCPublish");
            a.Num(3);
            a.Null();
            a.Str(key_);
            Command(0, a);
        }
        {
            Amf a;
            a.Str("createStream");
            a.Num(4);
            a.Null();
            Command(0, a);
            const auto v = WaitResult(4, "createStream");
            sid_ = v.size() > 3 && v[3].type == 0 ? static_cast<uint32_t>(v[3].num) : 1;
        }
        {
            Amf a;
            a.Str("publish");
            a.Num(5);
            a.Null();
            a.Str(key_);
            a.Str("live");
            Command(sid_, a);
            WaitPublishStart();
        }
    }

    void Unpublish()
    {
        Amf a;
        a.Str("FCUnpublish");
        a.Num(6);
        a.Null();
        a.Str(key_);
        Command(0, a);
        Amf d;
        d.Str("deleteStream");
        d.Num(7);
        d.Null();
        d.Num(sid_);
        Command(0, d);
    }

    uint32_t StreamId() const { return sid_; }

    void Send(uint8_t type, uint8_t csid, uint32_t sid, uint32_t ts, const uint8_t* d, size_t n)
    {
        out_.clear();
        const bool ext = ts >= 0xFFFFFF;
        out_.push_back(csid & 0x3F);  // fmt 0
        Be24(out_, ext ? 0xFFFFFF : ts);
        Be24(out_, static_cast<uint32_t>(n));
        out_.push_back(type);
        for (int i = 0; i < 4; ++i)
            out_.push_back(static_cast<uint8_t>(sid >> (i * 8)));  // little-endian
        if (ext)
            Be32(out_, ts);
        for (size_t pos = 0; pos < n;) {
            if (pos > 0) {
                out_.push_back(0xC0 | (csid & 0x3F));
                if (ext)
                    Be32(out_, ts);
            }
            const size_t k = std::min<size_t>(outChunk_, n - pos);
            out_.insert(out_.end(), d + pos, d + pos + k);
            pos += k;
        }
        t_.Write(out_.data(), out_.size());
    }

    // Разбирает входящее без ожидания: ping, подтверждения, onStatus с ошибкой.
    void Poll()
    {
        while (t_.Readable(0)) {
            const Msg m = ReadMessage();
            if (m.type == 20 || m.type == 17)
                CheckStatus(ParseCommand(m.data, m.type));
        }
    }

private:
    struct Msg {
        uint8_t type = 0;
        uint32_t sid = 0;
        std::vector<uint8_t> data;
    };
    struct InChunk {
        uint32_t ts = 0, delta = 0, len = 0, sid = 0;
        uint8_t type = 0;
        bool ext = false;
        std::vector<uint8_t> buf;
    };

    void Read(uint8_t* p, size_t n)
    {
        t_.ReadExact(p, n);
        received_ += n;
    }

    void Handshake()
    {
        std::vector<uint8_t> c0c1(1 + 1536);
        c0c1[0] = 3;
        std::mt19937 rng(static_cast<uint32_t>(QpcNow()));
        for (size_t i = 9; i < c0c1.size(); ++i)
            c0c1[i] = static_cast<uint8_t>(rng());
        t_.Write(c0c1.data(), c0c1.size());
        uint8_t s0 = 0;
        t_.ReadExact(&s0, 1);
        if (s0 != 3)
            throw std::runtime_error("сервер ответил не по протоколу RTMP");
        std::vector<uint8_t> s1(1536), s2(1536);
        t_.ReadExact(s1.data(), s1.size());
        t_.ReadExact(s2.data(), s2.size());
        t_.Write(s1.data(), s1.size());  // C2 = эхо S1
    }

    void Command(uint32_t sid, const Amf& a) { Send(20, 3, sid, 0, a.b.data(), a.b.size()); }

    Msg ReadMessage()
    {
        for (;;) {
            uint8_t b = 0;
            Read(&b, 1);
            const int fmt = b >> 6;
            uint32_t csid = b & 0x3F;
            if (csid == 0) {
                uint8_t x = 0;
                Read(&x, 1);
                csid = 64 + x;
            } else if (csid == 1) {
                uint8_t x[2]{};
                Read(x, 2);
                csid = 64 + x[0] + x[1] * 256u;
            }
            InChunk& c = in_[csid];
            const bool starting = c.buf.empty();
            uint8_t h[11]{};
            if (fmt == 0) {
                Read(h, 11);
                c.ts = RdBe24(h);
                c.len = RdBe24(h + 3);
                c.type = h[6];
                c.sid = h[7] | (h[8] << 8) | (h[9] << 16) | (static_cast<uint32_t>(h[10]) << 24);
                c.ext = c.ts == 0xFFFFFF;
                if (c.ext) {
                    Read(h, 4);
                    c.ts = RdBe32(h);
                }
                c.delta = 0;
            } else if (fmt == 1 || fmt == 2) {
                Read(h, fmt == 1 ? 7 : 3);
                c.delta = RdBe24(h);
                if (fmt == 1) {
                    c.len = RdBe24(h + 3);
                    c.type = h[6];
                }
                c.ext = c.delta == 0xFFFFFF;
                if (c.ext) {
                    Read(h, 4);
                    c.delta = RdBe32(h);
                }
                if (starting)
                    c.ts += c.delta;
            } else {
                if (c.ext)
                    Read(h, 4);
                if (starting)
                    c.ts += c.delta;
            }
            if (c.len > (16u << 20))
                throw std::runtime_error("сервер прислал слишком большое RTMP-сообщение");
            const size_t n = std::min<size_t>(inChunk_, c.len - c.buf.size());
            const size_t at = c.buf.size();
            c.buf.resize(at + n);
            if (n)
                Read(c.buf.data() + at, n);
            if (c.buf.size() < c.len)
                continue;

            Msg m{ c.type, c.sid, std::move(c.buf) };
            c.buf.clear();
            if (received_ - acked_ >= ackWindow_) {
                acked_ = received_;
                std::vector<uint8_t> p;
                Be32(p, static_cast<uint32_t>(received_));
                Send(3, 2, 0, 0, p.data(), p.size());
            }
            if (!HandleControl(m))
                return m;
        }
    }

    bool HandleControl(const Msg& m)
    {
        const auto& d = m.data;
        switch (m.type) {
        case 1:
            if (d.size() >= 4)
                inChunk_ = std::clamp<uint32_t>(RdBe32(d.data()) & 0x7FFFFFFF, 1, 1u << 24);
            return true;
        case 2:
        case 3:
            return true;
        case 4:
            if (d.size() >= 6 && RdBe16(d.data()) == 6) {  // PingRequest → PingResponse
                std::vector<uint8_t> p;
                Be16(p, 7);
                p.insert(p.end(), d.begin() + 2, d.begin() + 6);
                Send(4, 2, 0, 0, p.data(), p.size());
            }
            return true;
        case 5:
            if (d.size() >= 4)
                ackWindow_ = std::max<uint32_t>(RdBe32(d.data()), 4096);
            return true;
        case 6: {
            std::vector<uint8_t> p;
            Be32(p, 2500000);
            Send(5, 2, 0, 0, p.data(), p.size());
            return true;
        }
        default:
            return false;
        }
    }

    std::string Describe(const std::vector<AmfValue>& v) const
    {
        for (const auto& x : v) {
            if (x.type != 3 && x.type != 8)
                continue;
            std::string s = x.GetStr("description");
            if (s.empty())
                s = x.GetStr("code");
            if (!s.empty())
                return Redact(s, key_);
        }
        return "без описания";
    }

    std::vector<AmfValue> WaitResult(double txn, const char* what)
    {
        for (;;) {
            const Msg m = ReadMessage();
            if (m.type != 20 && m.type != 17)
                continue;
            auto v = ParseCommand(m.data, m.type);
            if (v.size() < 2 || v[0].type != 2)
                continue;
            if (v[1].type == 0 && v[1].num == txn) {
                if (v[0].str == "_result")
                    return v;
                if (v[0].str == "_error")
                    throw std::runtime_error(std::format("сервер отклонил {}: {}", what, Describe(v)));
            }
            CheckStatus(v);
        }
    }

    // true — публикация началась; ошибка — исключение.
    bool CheckStatus(const std::vector<AmfValue>& v)
    {
        if (v.empty() || v[0].type != 2)
            return false;
        if (v[0].str == "close")
            throw std::runtime_error("сервер закрыл трансляцию");
        if (v[0].str != "onStatus")
            return false;
        for (const auto& x : v) {
            if (x.type != 3 && x.type != 8)
                continue;
            const std::string code = x.GetStr("code");
            const std::string level = x.GetStr("level");
            if (code == "NetStream.Publish.Start")
                return true;
            if (level == "error" || code.find("Failed") != std::string::npos || code.find("BadName") != std::string::npos ||
                code.find("Rejected") != std::string::npos || code.find("Unpublish") != std::string::npos)
                throw std::runtime_error("сервер: " + Describe(v) + " — проверьте ключ потока");
        }
        return false;
    }

    void WaitPublishStart()
    {
        for (;;) {
            const Msg m = ReadMessage();
            if ((m.type == 20 || m.type == 17) && CheckStatus(ParseCommand(m.data, m.type)))
                return;
        }
    }

    NetTransport& t_;
    std::string key_;
    uint32_t outChunk_ = 128;
    uint32_t inChunk_ = 128;
    uint32_t ackWindow_ = 2500000;
    uint64_t received_ = 0;
    uint64_t acked_ = 0;
    std::map<uint32_t, InChunk> in_;
    uint32_t sid_ = 1;
    std::vector<uint8_t> out_;
};

std::vector<uint8_t> MetadataTag(const VideoStreamInfo& v, const std::optional<AudioStreamInfo>& a)
{
    Amf m;
    m.Str("@setDataFrame");
    m.Str("onMetaData");
    m.EcmaBegin(a ? 12 : 7);
    m.Key("duration");
    m.Num(0);
    m.Key("width");
    m.Num(v.width);
    m.Key("height");
    m.Num(v.height);
    m.Key("framerate");
    m.Num(v.fps);
    m.Key("videodatarate");
    m.Num(v.bitrateKbps);
    m.Key("videocodecid");
    m.Num(v.codec == Codec::H264 ? 7.0 : static_cast<double>(0x68766331));  // 'hvc1' для Enhanced RTMP
    m.Key("encoder");
    m.Str(GUVI_NAME " " GUVI_VERSION);
    if (a) {
        m.Key("audiocodecid");
        m.Num(10);
        m.Key("audiodatarate");
        m.Num(a->bitrateKbps);
        m.Key("audiosamplerate");
        m.Num(a->sampleRate);
        m.Key("audiosamplesize");
        m.Num(16);
        m.Key("stereo");
        m.Bool(a->channels >= 2);
    }
    m.ObjEnd();
    return m.b;
}

std::vector<uint8_t> AudioSpecificConfig(const AudioStreamInfo& a)
{
    // MF_MT_USER_DATA = 12 байт HEAACWAVEINFO + AudioSpecificConfig.
    if (a.aacUserData.size() > 12)
        return { a.aacUserData.begin() + 12, a.aacUserData.end() };
    return { 0x11, 0x90 };  // AAC-LC, 48 кГц, стерео
}

} // namespace

RtmpPublisher::RtmpPublisher(StreamTarget target, VideoStreamInfo video, std::vector<uint8_t> videoSequenceHeader,
                             std::optional<AudioStreamInfo> audio, int64_t basePts)
    : target_(std::move(target)), video_(video), videoHeader_(std::move(videoSequenceHeader)), audio_(std::move(audio)),
      basePts_(basePts)
{
    // ~2 с видео в очереди; больше — сеть не тянет, сбрасываем до ключевого кадра.
    maxQueuedBytes_ = std::max<size_t>(size_t{ video_.bitrateKbps } * 1000 / 8 * 2, 1u << 20);
    ParseUrl(target_.url);  // неверный адрес — ошибка сразу, до потока
    thread_ = std::thread(&RtmpPublisher::ThreadMain, this);
}

RtmpPublisher::~RtmpPublisher()
{
    {
        std::unique_lock lock(mtx_);
        stop_ = true;
        cv_.notify_all();
        // Даём сессии вежливо закончить публикацию; зависла в сети — рвём сокет.
        if (!cv_.wait_for(lock, std::chrono::milliseconds(1500), [&] { return finished_; }))
            if (auto* t = static_cast<NetTransport*>(transport_.load()))
                t->Abort();
    }
    if (thread_.joinable())
        thread_.join();
}

std::string RtmpPublisher::Error() const
{
    std::lock_guard lock(mtx_);
    return error_;
}

void RtmpPublisher::SetError(const std::string& e)
{
    std::lock_guard lock(mtx_);
    error_ = e;
}

void RtmpPublisher::PushVideo(const EncodedPacket& p)
{
    {
        std::lock_guard lock(mtx_);
        if (!accepting_)
            return;
        if (queuedBytes_ + p.data.size() > maxQueuedBytes_) {
            queue_.clear();
            queuedBytes_ = 0;
            flushed_ = true;
            keyRequest_ = true;
            ++drops_;
            if (!p.keyframe)
                return;
        }
        Item it;
        it.video = true;
        it.keyframe = p.keyframe;
        it.pts = p.pts;
        it.data.assign(p.data.begin(), p.data.end());
        queuedBytes_ += it.data.size();
        queue_.push_back(std::move(it));
    }
    cv_.notify_all();
}

void RtmpPublisher::PushAudio(const PacketPtr& p)
{
    if (!audio_ || p->pts < basePts_)
        return;
    {
        std::lock_guard lock(mtx_);
        if (!accepting_)
            return;
        Item it;
        it.pts = p->pts;
        it.data = p->data;
        queuedBytes_ += it.data.size();
        queue_.push_back(std::move(it));
    }
    cv_.notify_all();
}

void RtmpPublisher::Session()
{
    const RtmpUrl u = ParseUrl(target_.url);
    NetTransport t;
    struct Clear {
        RtmpPublisher* self;
        ~Clear()
        {
            std::lock_guard lock(self->mtx_);
            self->transport_ = nullptr;
            self->accepting_ = false;
            self->queue_.clear();
            self->queuedBytes_ = 0;
        }
    } clear{ this };
    {
        std::lock_guard lock(mtx_);
        if (stop_)
            return;
        transport_ = &t;
    }
    LogInfo("Трансляция: подключение к %s:%u (%s)", u.host.c_str(), static_cast<unsigned>(u.port),
            u.tls ? "RTMPS" : "RTMP");
    t.Open(u.host, u.port, u.tls, 10000);
    RtmpClient c(t, target_.key);
    c.Connect(u, video_.codec == Codec::HEVC);
    const uint32_t sid = c.StreamId();

    const auto meta = MetadataTag(video_, audio_);
    c.Send(18, 5, sid, 0, meta.data(), meta.size());
    const auto seq = SequenceTag(video_.codec, videoHeader_);
    c.Send(9, 6, sid, 0, seq.data(), seq.size());
    if (audio_) {
        std::vector<uint8_t> a = { 0xAF, 0x00 };
        const auto asc = AudioSpecificConfig(*audio_);
        a.insert(a.end(), asc.begin(), asc.end());
        c.Send(8, 4, sid, 0, a.data(), a.size());
    }

    {
        std::lock_guard lock(mtx_);
        queue_.clear();
        queuedBytes_ = 0;
        accepting_ = true;
        flushed_ = true;
        error_.clear();
    }
    keyRequest_ = true;
    reachedLive_ = true;
    state_ = StreamState::Live;
    LogInfo("● Трансляция в эфире: %s %ux%u@%u, %u кбит/с", video_.codec == Codec::H264 ? "H.264" : "HEVC",
            video_.width, video_.height, video_.fps, video_.bitrateKbps);

    const int64_t freq = QpcFrequency();
    int64_t windowStart = QpcNow();
    uint64_t windowBytes = 0;
    bool waitKey = true;
    std::deque<Item> batch;
    std::vector<uint8_t> tag;
    while (!stop_) {
        {
            std::unique_lock lock(mtx_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop_.load() || !queue_.empty(); });
            if (flushed_) {
                waitKey = true;
                flushed_ = false;
            }
            batch.swap(queue_);
            queuedBytes_ = 0;
        }
        for (const Item& it : batch) {
            const uint32_t ts = it.pts > basePts_ ? static_cast<uint32_t>((it.pts - basePts_) / 10000) : 0;
            if (it.video) {
                if (waitKey && !it.keyframe)
                    continue;
                waitKey = false;
                VideoTag(video_.codec, it.keyframe, it.data, tag);
                c.Send(9, 6, sid, ts, tag.data(), tag.size());
            } else {
                if (waitKey)
                    continue;
                tag = { 0xAF, 0x01 };
                tag.insert(tag.end(), it.data.begin(), it.data.end());
                c.Send(8, 4, sid, ts, tag.data(), tag.size());
            }
            windowBytes += tag.size();
        }
        batch.clear();
        c.Poll();

        const int64_t now = QpcNow();
        if (now - windowStart >= freq) {
            kbps_ = static_cast<double>(windowBytes) * 8.0 / 1000.0 * static_cast<double>(freq) /
                    static_cast<double>(now - windowStart);
            windowBytes = 0;
            windowStart = now;
        }
    }
    try {
        c.Unpublish();
    } catch (...) {
    }
}

void RtmpPublisher::ThreadMain()
{
    int attempt = 0;
    while (!stop_) {
        state_ = attempt == 0 ? StreamState::Connecting : StreamState::Reconnecting;
        reachedLive_ = false;
        try {
            Session();
        } catch (const std::exception& e) {
            if (!stop_) {
                const std::string msg = Redact(e.what(), target_.key);
                SetError(msg);
                LogWarn("Трансляция: %s", msg.c_str());
            }
        }
        kbps_ = 0.0;
        if (stop_)
            break;
        if (!target_.reconnect) {
            state_ = StreamState::Failed;
            break;
        }
        attempt = reachedLive_ ? 1 : attempt + 1;
        const int delay = std::min(2 << std::min(attempt - 1, 3), 15);
        state_ = StreamState::Reconnecting;
        LogInfo("Трансляция: переподключение через %d с", delay);
        std::unique_lock lock(mtx_);
        cv_.wait_for(lock, std::chrono::seconds(delay), [&] { return stop_.load(); });
    }
    if (state_ != StreamState::Failed)
        state_ = StreamState::Off;
    std::lock_guard lock(mtx_);
    finished_ = true;
    cv_.notify_all();
}
