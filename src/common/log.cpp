#include "common/log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace {

constexpr size_t kMaxLines = 200;

std::mutex g_mtx;
std::deque<LogLine> g_lines;
uint64_t g_version = 0;

void Write(LogLevel level, const char* fmt, va_list args)
{
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    std::string text = buf;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    std::FILE* out = level == LogLevel::Error ? stderr : stdout;
    std::fprintf(out, "%s\n", text.c_str());

    std::lock_guard lock(g_mtx);
    g_lines.push_back({ level, std::move(text) });
    if (g_lines.size() > kMaxLines)
        g_lines.pop_front();
    ++g_version;
}

} // namespace

#define LOG_IMPL(level)              \
    va_list args;                    \
    va_start(args, fmt);             \
    Write(level, fmt, args);         \
    va_end(args)

void LogInfo(const char* fmt, ...) { LOG_IMPL(LogLevel::Info); }
void LogWarn(const char* fmt, ...) { LOG_IMPL(LogLevel::Warn); }
void LogError(const char* fmt, ...) { LOG_IMPL(LogLevel::Error); }

std::vector<LogLine> RecentLog(size_t maxLines)
{
    std::lock_guard lock(g_mtx);
    const size_t n = std::min(maxLines, g_lines.size());
    return { g_lines.end() - static_cast<ptrdiff_t>(n), g_lines.end() };
}

uint64_t LogVersion()
{
    std::lock_guard lock(g_mtx);
    return g_version;
}
