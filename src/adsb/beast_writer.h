// Beast binary TCP output for ADS-B / Mode S frames (dump1090-compatible).
//
// Frame: 0x1A, type ('2' short / '3' long), 6-byte 12 MHz timestamp,
// 1-byte signal level, then the message bytes. Any 0x1A in the payload is
// escaped by doubling it.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

class BeastWriter
{
public:
    BeastWriter() = default;
    ~BeastWriter();

    bool start(int port);
    void stop();
    bool running() const { return listen_ != kInvalid; }
    int  port() const { return port_; }

    // Emits one Mode S frame. `tsSec` is epoch seconds; `sigLevel` is the
    // demodulator signal estimate (0..~1).
    void write(const uint8_t* bytes, int nbytes, double tsSec, double sigLevel);

    // Called periodically from the UI thread: accept new clients, drop dead
    // ones. Safe to call every frame.
    void poll();

    uint64_t sentCount() const { return sent_.load(); }
    int clientCount();

private:
    static constexpr uintptr_t kInvalid = ~(uintptr_t)0;

    void acceptClients();
    void closeAll();

    std::mutex mtx_;
    int port_ = 30005;
    uintptr_t listen_ = kInvalid;
    std::vector<uintptr_t> clients_;
    std::atomic<uint64_t> sent_{0};
};
