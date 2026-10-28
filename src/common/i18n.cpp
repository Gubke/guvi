#include "common/i18n.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::mutex g_mtx;
std::string g_lang = "ru";
std::unordered_map<std::string, std::string> g_map;
std::vector<std::string> g_frame;

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n)
        return L".";
    std::wstring path(buf, n);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";
    return path.substr(0, slash);
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WindowsLanguage()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        if (_wcsnicmp(name, L"ru", 2) == 0)
            return "ru";
        if (_wcsnicmp(name, L"uk", 2) == 0)
            return "uk";
        if (_wcsnicmp(name, L"pl", 2) == 0)
            return "pl";
    }
    return "en";
}

bool ParseString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"')
            return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= s.size())
            return false;
        const char e = s[i++];
        if (e == 'n')
            out += '\n';
        else if (e == 'r')
            out += '\r';
        else if (e == 't')
            out += '\t';
        else
            out += e;
    }
    return false;
}

void LoadFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF)
        text.erase(0, 3);
    size_t i = 0;
    auto ws = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
    };
    ws();
    if (i >= text.size() || text[i] != '{')
        return;
    ++i;
    while (i < text.size()) {
        ws();
        if (i < text.size() && text[i] == '}')
            break;
        std::string key, value;
        if (!ParseString(text, i, key))
            return;
        ws();
        if (i >= text.size() || text[i] != ':')
            return;
        ++i;
        ws();
        if (!ParseString(text, i, value))
            return;
        g_map[std::move(key)] = std::move(value);
        ws();
        if (i < text.size() && text[i] == ',')
            ++i;
    }
}

} // namespace

void I18nBeginFrame()
{
    g_frame.clear();
}

void I18nUse(const std::string& preference)
{
    std::string code = preference;
    if (code != "ru" && code != "uk" && code != "en" && code != "pl")
        code = WindowsLanguage();
    std::lock_guard lock(g_mtx);
    if (code == g_lang && (code == "ru" || !g_map.empty()))
        return;
    g_lang = code;
    g_map.clear();
    if (code == "ru")
        return;
    const auto path = ExeDir() + L"\\lang\\" + Utf8ToWide(code) + L".json";
    LoadFile(path);
}

const char* Tr(const char* russian)
{
    if (!russian || !*russian)
        return "";
    std::string value;
    {
        std::lock_guard lock(g_mtx);
        const auto it = g_map.find(russian);
        value = it == g_map.end() ? russian : it->second;
    }
    g_frame.push_back(std::move(value));
    return g_frame.back().c_str();
}

std::wstring TrW(const wchar_t* russian)
{
    if (!russian || !*russian)
        return {};
    const std::string key = WideToUtf8(russian);
    std::string value;
    {
        std::lock_guard lock(g_mtx);
        const auto it = g_map.find(key);
        value = it == g_map.end() ? key : it->second;
    }
    return Utf8ToWide(value);
}

std::string I18nLanguage()
{
    std::lock_guard lock(g_mtx);
    return g_lang;
}
