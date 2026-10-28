#pragma once

// Единственное место с названием и версией (заголовок окна, строка «о программе», README).
// Версия растёт на 0.1 при каждом заметном пользователю изменении: 0.3, 0.4, … 1.0.
#define GUVI_NAME "GuVi"
#define GUVI_VERSION "1.0"
// Публичный репозиторий для проверки обновлений. Токен не нужен и не хранится.
#define GUVI_GITHUB_REPO "gubke/guvi"

inline constexpr const char* kAppName = GUVI_NAME;
inline constexpr const char* kAppVersion = GUVI_VERSION;
inline constexpr const wchar_t* kAppTitleW = L"" GUVI_NAME;
inline constexpr const char* kGithubRepo = GUVI_GITHUB_REPO;
