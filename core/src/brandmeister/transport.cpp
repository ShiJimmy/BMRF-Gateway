#include "bmrf/brandmeister/transport.hpp"

#include "bmrf/common/log.hpp"

#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
#endif

namespace bmrf::brandmeister {

namespace {

constexpr const char* kTag = "BM/udp";

#if defined(_WIN32)
void ensureWsa()
{
    static bool initialised = false;
    if (!initialised) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        initialised = true;
    }
}

int socketError()
{
    return WSAGetLastError();
}
#else
int socketError()
{
    return errno;
}
#endif

socket_t toSocket(uintptr_t value)
{
    return static_cast<socket_t>(value);
}

uintptr_t fromSocket(socket_t value)
{
    return static_cast<uintptr_t>(value);
}

} // namespace

UdpTransport::UdpTransport()
{
#if defined(_WIN32)
    ensureWsa();
#endif
}

UdpTransport::~UdpTransport()
{
    close();
}

bool UdpTransport::open(const std::string& host, uint16_t port, std::string& error)
{
    close();
    m_peer.clear();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char portText[16];
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), portText, &hints, &result);
    if (rc != 0 || result == nullptr) {
        error = "failed to resolve " + host;
        logError(kTag, error);
        return false;
    }

    // Remember the resolved peer endpoint for display ("ip:port").
    {
        char hostText[NI_MAXHOST] = {0};
        char servText[NI_MAXSERV] = {0};
        if (::getnameinfo(result->ai_addr, static_cast<socklen_t>(result->ai_addrlen),
                          hostText, sizeof(hostText), servText, sizeof(servText),
                          NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            m_peer = std::string(hostText) + ":" + servText;
        }
    }

    socket_t handle = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (handle == kInvalidSocket) {
        freeaddrinfo(result);
        error = "socket() failed";
        logError(kTag, error);
        return false;
    }

    // Leave the socket unconnected (sendto/recvfrom). The native BrandMeister
    // protocol answers from a different source port than it receives on, so a
    // connect()ed socket would drop those replies.
    m_peerAddr.assign(reinterpret_cast<const uint8_t*>(result->ai_addr),
                      reinterpret_cast<const uint8_t*>(result->ai_addr) + result->ai_addrlen);
    m_peerAddrLen = static_cast<int>(result->ai_addrlen);
    freeaddrinfo(result);

#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(handle, FIONBIO, &nonBlocking);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif

    m_socket = fromSocket(handle);
    logInfo(kTag, "connected to udp://" + host + ":" + portText);
    return true;
}

void UdpTransport::close()
{
    if (m_socket == static_cast<uintptr_t>(-1))
        return;
    socket_t handle = toSocket(m_socket);
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
    m_socket = static_cast<uintptr_t>(-1);
    m_peerAddr.clear();
    m_peerAddrLen = 0;
}

bool UdpTransport::isOpen() const
{
    return m_socket != static_cast<uintptr_t>(-1);
}

bool UdpTransport::send(const uint8_t* data, size_t len)
{
    if (!isOpen() || m_peerAddr.empty())
        return false;
    const int sent = ::sendto(toSocket(m_socket), reinterpret_cast<const char*>(data),
                              static_cast<int>(len), 0,
                              reinterpret_cast<const sockaddr*>(m_peerAddr.data()), m_peerAddrLen);
    if (sent != static_cast<int>(len)) {
        logWarn(kTag, "send failed, error=" + std::to_string(socketError()));
        return false;
    }
    return true;
}

int UdpTransport::recv(uint8_t* data, size_t maxLen)
{
    if (!isOpen())
        return -1;
    const int received = ::recvfrom(toSocket(m_socket), reinterpret_cast<char*>(data),
                                    static_cast<int>(maxLen), 0, nullptr, nullptr);
    if (received < 0) {
        const int err = socketError();
#if defined(_WIN32)
        if (err == WSAEWOULDBLOCK)
            return 0;
#else
        if (err == EAGAIN || err == EWOULDBLOCK)
            return 0;
#endif
        return -1;
    }
    return received;
}

} // namespace bmrf::brandmeister
