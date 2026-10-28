#pragma once

#include <string>

// Язык интерфейса. Пустая строка в настройках — язык Windows (ru / uk / en / pl).
// Файлы lang/<код>.json лежат рядом с guvi.exe. Ключ — русская строка из интерфейса.

void I18nBeginFrame();
void I18nUse(const std::string& preference);

const char* Tr(const char* russian);
std::wstring TrW(const wchar_t* russian);
std::string I18nLanguage();
