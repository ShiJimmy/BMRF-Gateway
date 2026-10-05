#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bmrf::brandmeister {

// Transport abstraction so the Homebrew client can be unit tested without a
// real UDP socket (design doc §22: the whole stack must be testable offline).
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool open(const std::string& host, uint16_t port, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool send(const uint8_t* data, size_t len) = 0;

    // Non-blocking read of a single datagram.
    //   > 0 : number of bytes read
    //     0 : no datagram available
    //    -1 : error
    virtual int recv(uint8_t* data, size_t maxLen) = 0;

    // Resolved remote endpoint ("ip:port") as actually connected, if known.
    virtual std::string peerAddress() const { return {}; }
};

// Cross-platform UDP transport (POSIX sockets on Android/Linux, Winsock on
// Windows for development on the desktop).
class UdpTransport : public ITransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    bool open(const std::string& host, uint16_t port, std::string& error) override;
    void close() override;
    bool isOpen() const override;
    bool send(const uint8_t* data, size_t len) override;
    int recv(uint8_t* data, size_t maxLen) override;
    std::string peerAddress() const override { return m_peer; }

private:
    uintptr_t m_socket = static_cast<uintptr_t>(-1);
    std::string m_peer;
    // Resolved peer sockaddr, used with an unconnected socket (sendto); the
    // BrandMeister native protocol replies from a different source port, which a
    // connect()ed socket would drop.
    std::vector<uint8_t> m_peerAddr;
    int m_peerAddrLen = 0;
};

} // namespace bmrf::brandmeister
