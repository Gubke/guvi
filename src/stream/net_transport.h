#pragma once

#include <winsock2.h>
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// TCP-соединение с необязательным TLS (Schannel, проверка сертификата системой) — для rtmp:// и rtmps://.
// Блокирующие вызовы с таймаутами; Abort() из другого потока прерывает connect/send/recv.
class NetTransport {
public:
    NetTransport() = default;
    ~NetTransport();

    NetTransport(const NetTransport&) = delete;
    NetTransport& operator=(const NetTransport&) = delete;

    void Open(const std::string& host, uint16_t port, bool tls, int timeoutMs);
    void Write(const uint8_t* data, size_t size);
    // Возвращает > 0 байт; при закрытии / ошибке / таймауте бросает.
    size_t Read(uint8_t* data, size_t size);
    void ReadExact(uint8_t* data, size_t size);
    // Есть ли что читать (без ожидания при timeoutMs == 0).
    bool Readable(int timeoutMs);

    void Abort();
    void Close();

private:
    SOCKET Sock() const;
    void SendRaw(const uint8_t* data, size_t size);
    size_t RecvRaw(uint8_t* data, size_t size);
    void Handshake(const std::wstring& host);
    void Decrypt();  // inEnc_ → plain_

    std::atomic<SOCKET> sock_{ INVALID_SOCKET };
    bool tls_ = false;
    bool credValid_ = false;
    bool ctxValid_ = false;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    SecPkgContext_StreamSizes sizes_{};
    std::vector<uint8_t> inEnc_;   // принятые, ещё не расшифрованные TLS-записи
    std::vector<uint8_t> plain_;   // расшифрованные, ещё не отданные
    size_t plainPos_ = 0;
    std::wstring host_;
};
