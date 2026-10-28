#include "gui/settings.h"
#include "common/hr.h"

#include <shlobj.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <type_traits>
#include <variant>

namespace {

// Плоский JSON-объект: строки, числа, true/false. Больше настройкам не нужно.
using JsonValue = std::variant<std::string, double, bool>;
using JsonObject = std::map<std::string, JsonValue>;

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Escape(const std::string& s)
{
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else
                out += c;
        }
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool ParseObject(JsonObject& out)
    {
        Ws();
        if (!Eat('{'))
            return false;
        Ws();
        if (Eat('}'))
            return true;
        for (;;) {
            std::string key;
            Ws();
            if (!ParseString(key))
                return false;
            Ws();
            if (!Eat(':'))
                return false;
            Ws();
            JsonValue v;
            if (!ParseValue(v))
                return false;
            out[key] = std::move(v);
            Ws();
            if (Eat(','))
                continue;
            return Eat('}');
        }
    }

private:
    void Ws()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }

    bool Eat(char c)
    {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool ParseString(std::string& out)
    {
        if (!Eat('"'))
            return false;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return false;
            const char e = s_[i_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i_ + 4 > s_.size())
                    return false;
                const auto cp = static_cast<unsigned>(std::strtoul(s_.substr(i_, 4).c_str(), nullptr, 16));
                i_ += 4;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }

    bool ParseValue(JsonValue& v)
    {
        if (i_ >= s_.size())
            return false;
        if (s_[i_] == '"') {
            std::string str;
            if (!ParseString(str))
                return false;
            v = std::move(str);
            return true;
        }
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            v = true;
            return true;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            v = false;
            return true;
        }
        char* end = nullptr;
        const double d = std::strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_)
            return false;
        i_ = static_cast<size_t>(end - s_.c_str());
        v = d;
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
};

template <class T>
void Get(const JsonObject& o, const char* key, T& out)
{
    const auto it = o.find(key);
    if (it == o.end())
        return;
    if constexpr (std::is_same_v<T, bool>) {
        if (auto* b = std::get_if<bool>(&it->second))
            out = *b;
    } else if constexpr (std::is_same_v<T, std::wstring>) {
        if (auto* s = std::get_if<std::string>(&it->second))
            out = Utf8ToWide(*s);
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (auto* s = std::get_if<std::string>(&it->second))
            out = *s;
    } else {
        if (auto* d = std::get_if<double>(&it->second))
            out = static_cast<T>(*d);
    }
}

// Ключ трансляции: DPAPI (CurrentUser) → base64. Чужой пользователь / другая машина не расшифруют.
std::string ProtectSecret(const std::string& plain)
{
    if (plain.empty())
        return {};
    DATA_BLOB in{ static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"GuVi stream key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    DWORD len = 0;
    std::string b64;
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len)) {
        b64.resize(len);
        CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
        b64.resize(len);
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b64;
}

std::string UnprotectSecret(const std::string& b64)
{
    if (b64.empty())
        return {};
    DWORD len = 0;
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr))
        return {};
    std::vector<BYTE> blob(len);
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, blob.data(), &len, nullptr, nullptr))
        return {};
    DATA_BLOB in{ len, blob.data() };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

std::wstring DefaultOutDir()
{
    PWSTR videos = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)))
        dir = (std::filesystem::path(videos) / L"Recorder").wstring();
    CoTaskMemFree(videos);
    return dir.empty() ? L"." : dir;
}

} // namespace

std::wstring SettingsPath()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / L"recorder_gui.json").wstring();
}

GuiSettings LoadSettings()
{
    GuiSettings s;
    s.outDir = DefaultOutDir();

    std::ifstream in(std::filesystem::path(SettingsPath()), std::ios::binary);
    if (!in)
        return s;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    JsonObject o;
    if (!Parser(text).ParseObject(o))
        return s;

    Get(o, "sourceIsWindow", s.sourceIsWindow);
    Get(o, "monitor", s.monitor);
    Get(o, "windowTitle", s.windowTitle);
    Get(o, "codec", s.codec);
    Get(o, "encoder", s.encoder);
    Get(o, "fps", s.fps);
    Get(o, "bitrateKbps", s.bitrateKbps);
    Get(o, "keyintSeconds", s.keyintSeconds);
    Get(o, "cursor", s.cursor);
    Get(o, "systemAudio", s.systemAudio);
    Get(o, "microphone", s.microphone);
    Get(o, "micGain", s.micGain);
    Get(o, "audioBitrateKbps", s.audioBitrateKbps);
    Get(o, "renderDeviceId", s.renderDeviceId);
    Get(o, "captureDeviceId", s.captureDeviceId);
    Get(o, "replaySeconds", s.replaySeconds);
    Get(o, "outDir", s.outDir);
    Get(o, "overlay", s.overlay);
    Get(o, "outputHeight", s.outputHeight);
    Get(o, "systemGain", s.systemGain);
    Get(o, "librarySort", s.librarySort);
    s.outputHeight = std::clamp(s.outputHeight, 0, 4320);
    s.systemGain = std::clamp(s.systemGain, 0.0f, 4.0f);
    s.librarySort = std::clamp(s.librarySort, 0, 3);
    Get(o, "webcam", s.webcam);
    Get(o, "webcamDeviceId", s.webcamDeviceId);
    Get(o, "webcamSize", s.webcamSize);
    Get(o, "webcamX", s.webcamX);
    Get(o, "webcamY", s.webcamY);
    Get(o, "webcamMode", s.webcamMode);
    s.webcamSize = std::clamp(s.webcamSize, 5.0f, 100.0f);
    s.webcamX = std::clamp(s.webcamX, 0.0f, 100.0f);
    s.webcamY = std::clamp(s.webcamY, 0.0f, 100.0f);
    s.webcamMode = 0;
    Get(o, "preset", s.preset);
    Get(o, "systemMuted", s.systemMuted);
    Get(o, "micMuted", s.micMuted);
    Get(o, "hkRecordVk", s.hkRecordVk);
    Get(o, "hkRecordMods", s.hkRecordMods);
    Get(o, "hkReplayVk", s.hkReplayVk);
    Get(o, "hkReplayMods", s.hkReplayMods);
    Get(o, "hkShotVk", s.hkShotVk);
    Get(o, "hkShotMods", s.hkShotMods);
    Get(o, "hkStreamVk", s.hkStreamVk);
    Get(o, "hkStreamMods", s.hkStreamMods);
    Get(o, "hkPauseVk", s.hkPauseVk);
    Get(o, "hkPauseMods", s.hkPauseMods);
    Get(o, "widgetCount", s.widgetCount);
    s.widgetCount = std::clamp(s.widgetCount, 0, kMaxStreamWidgets);
    for (int i = 0; i < kMaxStreamWidgets; ++i) {
        const std::string on = std::format("w{}on", i);
        const std::string url = std::format("w{}url", i);
        const std::string size = std::format("w{}size", i);
        const std::string x = std::format("w{}x", i);
        const std::string y = std::format("w{}y", i);
        Get(o, on.c_str(), s.widgets[i].enabled);
        Get(o, url.c_str(), s.widgets[i].url);
        Get(o, size.c_str(), s.widgets[i].size);
        Get(o, x.c_str(), s.widgets[i].x);
        Get(o, y.c_str(), s.widgets[i].y);
        s.widgets[i].size = std::clamp(s.widgets[i].size, 5.0f, 100.0f);
        s.widgets[i].x = std::clamp(s.widgets[i].x, 0.0f, 100.0f);
        s.widgets[i].y = std::clamp(s.widgets[i].y, 0.0f, 100.0f);
    }
    Get(o, "streamUrl", s.streamUrl);
    {
        std::string protectedKey;
        Get(o, "streamKeyDpapi", protectedKey);
        s.streamKey = UnprotectSecret(protectedKey);
    }
    Get(o, "streamCodec", s.streamCodec);
    Get(o, "streamHeight", s.streamHeight);
    Get(o, "streamFps", s.streamFps);
    Get(o, "streamBitrateKbps", s.streamBitrateKbps);
    Get(o, "streamReconnect", s.streamReconnect);
    Get(o, "streamAlsoRecord", s.streamAlsoRecord);
    Get(o, "language", s.language);
    s.streamCodec = std::clamp(s.streamCodec, 0, 1);
    s.streamHeight = std::clamp(s.streamHeight, 144, 2160);
    s.streamFps = std::clamp(s.streamFps, 10, 120);
    s.streamBitrateKbps = std::clamp(s.streamBitrateKbps, 300, 50000);
    s.preset = std::clamp(s.preset, 0, 1);
    for (int* vk : { &s.hkRecordVk, &s.hkReplayVk, &s.hkShotVk, &s.hkStreamVk, &s.hkPauseVk })
        *vk = std::clamp(*vk, 0, 0xFE);
    for (int* m : { &s.hkRecordMods, &s.hkReplayMods, &s.hkShotMods, &s.hkStreamMods, &s.hkPauseMods })
        *m &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;

    s.codec = std::clamp(s.codec, 0, 1);
    s.encoder = std::clamp(s.encoder, 0, 3);
    s.fps = std::clamp(s.fps, 1, 240);
    s.bitrateKbps = std::clamp(s.bitrateKbps, 1000, 200000);
    s.keyintSeconds = std::clamp(s.keyintSeconds, 1, 10);
    s.micGain = std::clamp(s.micGain, 0.0f, 8.0f);
    s.replaySeconds = std::clamp(s.replaySeconds, 0, 600);
    if (s.audioBitrateKbps != 96 && s.audioBitrateKbps != 128 && s.audioBitrateKbps != 160)
        s.audioBitrateKbps = 192;
    if (s.outDir.empty())
        s.outDir = DefaultOutDir();
    return s;
}

bool SaveSettings(const GuiSettings& s)
{
    auto str = [](const std::wstring& w) { return "\"" + Escape(WideToUtf8(w)) + "\""; };
    auto boolean = [](bool b) { return b ? "true" : "false"; };

    std::string json = "{\n";
    json += std::format("  \"sourceIsWindow\": {},\n", boolean(s.sourceIsWindow));
    json += std::format("  \"monitor\": {},\n", s.monitor);
    json += std::format("  \"windowTitle\": {},\n", str(s.windowTitle));
    json += std::format("  \"codec\": {},\n", s.codec);
    json += std::format("  \"encoder\": {},\n", s.encoder);
    json += std::format("  \"fps\": {},\n", s.fps);
    json += std::format("  \"bitrateKbps\": {},\n", s.bitrateKbps);
    json += std::format("  \"keyintSeconds\": {},\n", s.keyintSeconds);
    json += std::format("  \"cursor\": {},\n", boolean(s.cursor));
    json += std::format("  \"systemAudio\": {},\n", boolean(s.systemAudio));
    json += std::format("  \"microphone\": {},\n", boolean(s.microphone));
    json += std::format("  \"micGain\": {:.2f},\n", s.micGain);
    json += std::format("  \"audioBitrateKbps\": {},\n", s.audioBitrateKbps);
    json += std::format("  \"renderDeviceId\": {},\n", str(s.renderDeviceId));
    json += std::format("  \"captureDeviceId\": {},\n", str(s.captureDeviceId));
    json += std::format("  \"replaySeconds\": {},\n", s.replaySeconds);
    json += std::format("  \"overlay\": {},\n", boolean(s.overlay));
    json += std::format("  \"outputHeight\": {},\n", s.outputHeight);
    json += std::format("  \"systemGain\": {:.2f},\n", s.systemGain);
    json += std::format("  \"librarySort\": {},\n", s.librarySort);
    json += std::format("  \"webcam\": {},\n", boolean(s.webcam));
    json += std::format("  \"webcamDeviceId\": {},\n", str(s.webcamDeviceId));
    json += std::format("  \"webcamSize\": {:.2f},\n", s.webcamSize);
    json += std::format("  \"webcamX\": {:.2f},\n", s.webcamX);
    json += std::format("  \"webcamY\": {:.2f},\n", s.webcamY);
    json += std::format("  \"webcamMode\": {},\n", s.webcamMode);
    json += std::format("  \"preset\": {},\n", s.preset);
    json += std::format("  \"systemMuted\": {},\n", boolean(s.systemMuted));
    json += std::format("  \"micMuted\": {},\n", boolean(s.micMuted));
    json += std::format("  \"hkRecordVk\": {},\n  \"hkRecordMods\": {},\n", s.hkRecordVk, s.hkRecordMods);
    json += std::format("  \"hkReplayVk\": {},\n  \"hkReplayMods\": {},\n", s.hkReplayVk, s.hkReplayMods);
    json += std::format("  \"hkShotVk\": {},\n  \"hkShotMods\": {},\n", s.hkShotVk, s.hkShotMods);
    json += std::format("  \"hkStreamVk\": {},\n  \"hkStreamMods\": {},\n", s.hkStreamVk, s.hkStreamMods);
    json += std::format("  \"hkPauseVk\": {},\n  \"hkPauseMods\": {},\n", s.hkPauseVk, s.hkPauseMods);
    json += std::format("  \"widgetCount\": {},\n", s.widgetCount);
    for (int i = 0; i < kMaxStreamWidgets; ++i) {
        const auto& w = s.widgets[i];
        json += std::format("  \"w{}on\": {},\n", i, boolean(w.enabled));
        json += std::format("  \"w{}url\": \"{}\",\n", i, Escape(w.url));
        json += std::format("  \"w{}size\": {:.2f},\n", i, w.size);
        json += std::format("  \"w{}x\": {:.2f},\n", i, w.x);
        json += std::format("  \"w{}y\": {:.2f},\n", i, w.y);
    }
    json += std::format("  \"streamUrl\": \"{}\",\n", Escape(s.streamUrl));
    json += std::format("  \"streamKeyDpapi\": \"{}\",\n", ProtectSecret(s.streamKey));
    json += std::format("  \"streamCodec\": {},\n", s.streamCodec);
    json += std::format("  \"streamHeight\": {},\n", s.streamHeight);
    json += std::format("  \"streamFps\": {},\n", s.streamFps);
    json += std::format("  \"streamBitrateKbps\": {},\n", s.streamBitrateKbps);
    json += std::format("  \"streamReconnect\": {},\n", boolean(s.streamReconnect));
    json += std::format("  \"streamAlsoRecord\": {},\n", boolean(s.streamAlsoRecord));
    json += std::format("  \"language\": \"{}\",\n", Escape(s.language));
    json += std::format("  \"outDir\": {}\n", str(s.outDir));
    json += "}\n";

    std::ofstream out(std::filesystem::path(SettingsPath()), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << json;
    return static_cast<bool>(out);
}
