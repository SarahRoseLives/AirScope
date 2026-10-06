// Thread-safe single-producer / single-consumer IQ ring buffer.
// Producer: SDR read thread pushes interleaved float IQ.
// Consumer: UI/FFT thread pulls the most recent N complex samples.
#pragma once

#include <complex>
#include <cstddef>
#include <mutex>
#include <vector>

class IqRing
{
public:
    explicit IqRing(size_t capacityComplex = (1u << 20))
        : cap_(capacityComplex), buf_(capacityComplex * 2, 0.0f)
    {
    }

    // Push nComplex interleaved (I,Q) float pairs.
    void push(const float* iq, size_t nComplex)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < nComplex; ++i)
        {
            size_t w = (write_ % cap_) * 2;
            buf_[w]     = iq[i * 2];
            buf_[w + 1] = iq[i * 2 + 1];
            ++write_;
        }
        total_ += nComplex;
    }

    size_t available() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return total_ < cap_ ? total_ : cap_;
    }

    // Copy the most recent n complex samples (chronological order) as
    // std::complex<double>. Returns false if fewer than n are available.
    bool latest(std::complex<double>* out, size_t n)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (n > cap_ || total_ < n)
            return false;
        size_t start = write_ - n; // unsigned wrap is fine modulo cap_
        for (size_t i = 0; i < n; ++i)
        {
            size_t idx = ((start + i) % cap_) * 2;
            out[i] = std::complex<double>(buf_[idx], buf_[idx + 1]);
        }
        return true;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        write_ = 0;
        total_ = 0;
        std::fill(buf_.begin(), buf_.end(), 0.0f);
    }

private:
    mutable std::mutex mtx_;
    size_t cap_;
    std::vector<float> buf_;
    size_t write_ = 0;
    size_t total_ = 0;
};
