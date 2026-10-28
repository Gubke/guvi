#include "stream/net_transport.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>

namespace {

void EnsureWinsock()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            throw std::runtime_error("WSAStartup не удался");
    });
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

[[noreturn]] void ThrowSec(const char* what, SECURITY_STATUS s)
{
    throw std::runtime_error(std::format("TLS: {} (0x{:08X})", what, static_cast<unsigned>(s)));
}

constexpr ULONG kIscFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;

} // namespace

NetTransport::~NetTransport()
{
    Close();
}

SOCKET NetTransport::Sock() const
{
    const SOCKET s = sock_.load();
    if (s == INVALID_SOCKET)
        throw std::runtime_error("соединение закрыто");
    return s;
}

void NetTransport::Open(const std::string& host, uint16_t port, bool tls, int timeoutMs)
{
    EnsureWinsock();
    Close();
    tls_ = tls;
    host_ = Widen(host);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list)
        throw std::runtime_error("не удалось найти сервер " + host);

    int lastError = 0;
    for (addrinfo* a = list; a; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        sock_ = s;
        // Неблокирующий connect с таймаутом, затем обратно в блокирующий режим.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        bool ok = connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            ok = select(0, nullptr, &wr, &ex, &tv) == 1 && FD_ISSET(s, &wr);
        }
        if (ok) {
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            break;
        }
        lastError = WSAGetLastError();
        const SOCKET mine = sock_.exchange(INVALID_SOCKET);
        if (mine != INVALID_SOCKET)
            closesocket(mine);
    }
    freeaddrinfo(list);
    if (sock_.load() == INVALID_SOCKET)
        throw std::runtime_error(std::format("не удалось подключиться к {}:{} (ошибка {})", host, port, lastError));

    const SOCKET s = Sock();
    const DWORD to = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    const BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    const int sndBuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof(sndBuf));

    if (tls_)
        Handshake(host_);
}

void NetTransport::SendRaw(const uint8_t* data, size_t size)
{
    while (size > 0) {
        const int n = send(Sock(), reinterpret_cast<const char*>(data), static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0)
            throw std::runtime_error(std::format("сеть: отправка не удалась (ошибка {})", WSAGetLastError()));
        data += n;
        size -= static_cast<size_t>(n);
    }
}

size_t NetTransport::RecvRaw(uint8_t* data, size_t size)
{
    const int n = recv(Sock(), reinterpret_cast<char*>(data), static_cast<int>(size), 0);
    if (n == 0)
        throw std::runtime_error("сервер закрыл соединение");
    if (n < 0) {
        const int e = WSAGetLastError();
        throw std::runtime_error(e == WSAETIMEDOUT ? std::string("сервер не отвечает (таймаут)")
                                                   : std::format("сеть: приём не удался (ошибка {})", e));
    }
    return static_cast<size_t>(n);
}

void NetTransport::Handshake(const std::wstring& host)
{
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                                   nullptr, &sc, nullptr, nullptr, &cred_, nullptr);
    if (st != SEC_E_OK)
        ThrowSec("AcquireCredentialsHandle", st);
    credValid_ = true;

    auto sendToken = [&](SecBuffer& b) {
        if (b.cbBuffer && b.pvBuffer) {
            SendRaw(static_cast<const uint8_t*>(b.pvBuffer), b.cbBuffer);
            FreeContextBuffer(b.pvBuffer);
            b.pvBuffer = nullptr;
        }
    };

    ULONG outFlags = 0;
    {
        SecBuffer out{ 0, SECBUFFER_TOKEN, nullptr };
        SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, &out };
        st = InitializeSecurityContextW(&cred_, nullptr, const_cast<wchar_t*>(host.c_str()), kIscFlags, 0, 0, nullptr,
                                        0, &ctx_, &outDesc, &outFlags, nullptr);
        if (st != SEC_I_CONTINUE_NEEDED)
            ThrowSec("InitializeSecurityContext", st);
        ctxValid_ = true;
        sendToken(out);
    }

    std::vector<uint8_t> buf;
    uint8_t tmp[16384];
    bool needRead = true;
    for (;;) {
        if (needRead) {
            const size_t n = RecvRaw(tmp, sizeof(tmp));
            buf.insert(buf.end(), tmp, tmp + n);
        }
        SecBuffer in[2] = { { static_cast<ULONG>(buf.size()), SECBUFFER_TOKEN, buf.data() },
                            { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, in };
        SecBuffer out[1] = { { 0, SECBUFFER_TOKEN, nullptr } };
        SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, out };
        st = InitializeSecurityContextW(&cred_, &ctx_, const_cast<wchar_t*>(host.c_str()), kIscFlags, 0, 0, &inDesc,
                                        0, nullptr, &outDesc, &outFlags, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            needRead = true;
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) {
            if (out[0].pvBuffer)
                FreeContextBuffer(out[0].pvBuffer);
            ThrowSec(st == SEC_E_UNTRUSTED_ROOT || st == SEC_E_CERT_UNKNOWN || st == SEC_E_WRONG_PRINCIPAL
                         ? "сертификат сервера не прошёл проверку"
                         : "рукопожатие не удалось",
                     st);
        }
        sendToken(out[0]);
        if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0) {
            std::vector<uint8_t> extra(buf.end() - in[1].cbBuffer, buf.end());
            buf.swap(extra);
            needRead = false;
        } else {
            buf.clear();
            needRead = true;
        }
        if (st == SEC_E_OK)
            break;
    }
    inEnc_ = std::move(buf);
    st = QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
    if (st != SEC_E_OK)
        ThrowSec("QueryContextAttributes", st);
}

void NetTransport::Write(const uint8_t* data, size_t size)
{
    if (!tls_) {
        SendRaw(data, size);
        return;
    }
    std::vector<uint8_t> rec(sizes_.cbHeader + sizes_.cbMaximumMessage + sizes_.cbTrailer);
    while (size > 0) {
        const size_t n = std::min<size_t>(size, sizes_.cbMaximumMessage);
        std::memcpy(rec.data() + sizes_.cbHeader, data, n);
        SecBuffer b[4] = { { sizes_.cbHeader, SECBUFFER_STREAM_HEADER, rec.data() },
                           { static_cast<ULONG>(n), SECBUFFER_DATA, rec.data() + sizes_.cbHeader },
                           { sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, rec.data() + sizes_.cbHeader + n },
                           { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, b };
        const SECURITY_STATUS st = EncryptMessage(&ctx_, 0, &desc, 0);
        if (st != SEC_E_OK)
            ThrowSec("EncryptMessage", st);
        SendRaw(rec.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        data += n;
        size -= n;
    }
}

void NetTransport::Decrypt()
{
    for (;;) {
        if (inEnc_.empty()) {
            uint8_t tmp[16384];
            const size_t n = RecvRaw(tmp, sizeof(tmp));
            inEnc_.insert(inEnc_.end(), tmp, tmp + n);
        }
        SecBuffer b[4] = { { static_cast<ULONG>(inEnc_.size()), SECBUFFER_DATA, inEnc_.data() },
                           { 0, SECBUFFER_EMPTY, nullptr },
                           { 0, SECBUFFER_EMPTY, nullptr },
                           { 0, SECBUFFER_EMPTY, nullptr } };
        SecBufferDesc desc{ SECBUFFER_VERSION, 4, b };
        const SECURITY_STATUS st = DecryptMessage(&ctx_, &desc, 0, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            uint8_t tmp[16384];
            const size_t n = RecvRaw(tmp, sizeof(tmp));
            inEnc_.insert(inEnc_.end(), tmp, tmp + n);
            continue;
        }
        if (st == SEC_I_CONTEXT_EXPIRED)
            throw std::runtime_error("сервер закрыл TLS-соединение");
        if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE)
            ThrowSec("DecryptMessage", st);

        std::vector<uint8_t> extra;
        bool gotData = false;
        for (const SecBuffer& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer > 0) {
                const auto* p = static_cast<const uint8_t*>(x.pvBuffer);
                plain_.insert(plain_.end(), p, p + x.cbBuffer);
                gotData = true;
            } else if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer > 0) {
                const auto* p = static_cast<const uint8_t*>(x.pvBuffer);
                extra.assign(p, p + x.cbBuffer);
            }
        }
        inEnc_.swap(extra);

        if (st == SEC_I_RENEGOTIATE) {
            // TLS 1.3 post-handshake (билеты сессии): отдать остаток обратно в контекст.
            SecBuffer in[2] = { { static_cast<ULONG>(inEnc_.size()), SECBUFFER_TOKEN, inEnc_.data() },
                                { 0, SECBUFFER_EMPTY, nullptr } };
            SecBufferDesc inDesc{ SECBUFFER_VERSION, 2, in };
            SecBuffer out[1] = { { 0, SECBUFFER_TOKEN, nullptr } };
            SecBufferDesc outDesc{ SECBUFFER_VERSION, 1, out };
            ULONG flags = 0;
            const SECURITY_STATUS r = InitializeSecurityContextW(&cred_, &ctx_, const_cast<wchar_t*>(host_.c_str()),
                                                                 kIscFlags, 0, 0, inEnc_.empty() ? nullptr : &inDesc,
                                                                 0, nullptr, &outDesc, &flags, nullptr);
            if (out[0].pvBuffer) {
                if (out[0].cbBuffer)
                    SendRaw(static_cast<const uint8_t*>(out[0].pvBuffer), out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
            }
            if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
                ThrowSec("пересогласование", r);
            if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0)
                inEnc_.assign(inEnc_.end() - in[1].cbBuffer, inEnc_.end());
            else
                inEnc_.clear();
        }
        if (gotData)
            return;
    }
}

size_t NetTransport::Read(uint8_t* data, size_t size)
{
    if (!tls_)
        return RecvRaw(data, size);
    if (plainPos_ >= plain_.size()) {
        plain_.clear();
        plainPos_ = 0;
        Decrypt();
    }
    const size_t n = std::min(size, plain_.size() - plainPos_);
    std::memcpy(data, plain_.data() + plainPos_, n);
    plainPos_ += n;
    return n;
}

void NetTransport::ReadExact(uint8_t* data, size_t size)
{
    while (size > 0) {
        const size_t n = Read(data, size);
        data += n;
        size -= n;
    }
}

bool NetTransport::Readable(int timeoutMs)
{
    if (tls_ && (plainPos_ < plain_.size() || !inEnc_.empty()))
        return true;
    const SOCKET s = Sock();
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    return select(0, &rd, nullptr, nullptr, &tv) == 1;
}

void NetTransport::Abort()
{
    const SOCKET s = sock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

void NetTransport::Close()
{
    Abort();
    if (ctxValid_) {
        DeleteSecurityContext(&ctx_);
        ctxValid_ = false;
    }
    if (credValid_) {
        FreeCredentialsHandle(&cred_);
        credValid_ = false;
    }
    inEnc_.clear();
    plain_.clear();
    plainPos_ = 0;
}
