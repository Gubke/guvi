#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class LogLevel { Info, Warn, Error };

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string text;  // UTF-8, без завершающего \n
};

// printf-стиль. Пишет в stdout/stderr (CLI) и в кольцевой журнал, который показывает GUI.
void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

std::vector<LogLine> RecentLog(size_t maxLines);
uint64_t LogVersion();  // растёт при каждой новой строке
