#pragma once

#include <windows.h>

#include <format>
#include <stdexcept>
#include <string>

inline void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        throw std::runtime_error(std::format("{}: HRESULT 0x{:08X}", what, static_cast<unsigned>(hr)));
}

std::string WideToUtf8(std::wstring_view s);
