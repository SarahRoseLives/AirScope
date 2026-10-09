#include "adsb/beast_writer.h"

#include <cmath>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCKET (-1)
#define CLOSESOCK ::close
#endif

namespace
{
#if defined(_WIN32)
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };
static WsaInit g_wsa;
#endif

void setNonBlocking(socket_t s)
{
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

bool wouldBlock()
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
} // namespace

BeastWriter::~BeastWriter()
{
    stop();
}

bool BeastWriter::start(int port)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (listen_ != kInvalid)
        return true;
    port_ = port;

    socket_t s = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (s != INVALID_SOCKET)
    {
        int off = 0;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in6 a6{};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons((uint16_t)port_);
        if (::bind(s, (sockaddr*)&a6, sizeof(a6)) == 0 && ::listen(s, 8) == 0)
        {
            setNonBlocking(s);
            listen_ = (uintptr_t)s;
            return true;
        }
        CLOSESOCK(s);
    }

    s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port_);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 8) != 0)
    {
        CLOSESOCK(s);
        return false;
    }
    setNonBlocking(s);
    listen_ = (uintptr_t)s;
    return true;
}

void BeastWriter::stop()
{
    std::lock_guard<std::mutex> lk(mtx_);
    closeAll();
}

void BeastWriter::closeAll()
{
    for (uintptr_t c : clients_)
        CLOSESOCK((socket_t)c);
    clients_.clear();
    if (listen_ != kInvalid)
    {
        CLOSESOCK((socket_t)listen_);
        listen_ = kInvalid;
    }
}

void BeastWriter::acceptClients()
{
    if (listen_ == kInvalid) return;
    for (;;)
    {
        socket_t c = ::accept((socket_t)listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        setNonBlocking(c);
        clients_.push_back((uintptr_t)c);
    }
}

void BeastWriter::poll()
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (listen_ == kInvalid) return;
    acceptClients();

    char tmp[8];
    for (size_t i = 0; i < clients_.size();)
    {
        int r = ::recv((socket_t)clients_[i], tmp, sizeof(tmp), MSG_PEEK);
        if (r == 0 || (r < 0 && !wouldBlock()))
        {
            CLOSESOCK((socket_t)clients_[i]);
            clients_.erase(clients_.begin() + i);
        }
        else
        {
            ++i;
        }
    }
}

int BeastWriter::clientCount()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return (int)clients_.size();
}

void BeastWriter::write(const uint8_t* bytes, int nbytes, double tsSec, double sigLevel)
{
    if (nbytes <= 0)
        return;

    uint8_t type = (nbytes > 7) ? 0x33 : 0x32; // '3' long, '2' short
    uint64_t ts = (uint64_t)(tsSec * 12000000.0) & 0xFFFFFFFFFFFFULL;
    int sig = (int)(std::sqrt(sigLevel > 0 ? sigLevel : 0.0) * 255.0);
    if (sig > 255) sig = 255;

    // Every byte after the type is escaped: 0x1A is the frame delimiter, so an
    // unescaped 0x1A in the timestamp, signal or payload desyncs the reader.
    uint8_t body[6 + 1 + 14];
    int bn = 0;
    for (int i = 5; i >= 0; --i)
        body[bn++] = (uint8_t)((ts >> (i * 8)) & 0xFF);
    body[bn++] = (uint8_t)sig;
    for (int i = 0; i < nbytes; ++i)
        body[bn++] = bytes[i];

    uint8_t frame[2 + (6 + 1 + 14) * 2];
    int n = 0;
    frame[n++] = 0x1A;
    frame[n++] = type;
    for (int i = 0; i < bn; ++i)
    {
        frame[n++] = body[i];
        if (body[i] == 0x1A)
            frame[n++] = 0x1A; // escape
    }

    std::lock_guard<std::mutex> lk(mtx_);
    if (clients_.empty())
        return;
    for (size_t i = 0; i < clients_.size();)
    {
        int r = ::send((socket_t)clients_[i], (const char*)frame, n, 0);
        if (r < 0 && !wouldBlock())
        {
            CLOSESOCK((socket_t)clients_[i]);
            clients_.erase(clients_.begin() + i);
        }
        else
        {
            ++i;
        }
    }
    sent_.fetch_add(1);
}
