#include "voice/wav_writer.h"

#include <cstdio>
#include <cstring>
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

namespace
{
void put32(std::FILE* f, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v), (uint8_t)(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
} // namespace

// ---- OGG Vorbis encoder (libvorbis + libogg) ----

struct WavWriterOggImpl
{
    vorbis_info vi{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    vorbis_comment vc{};
    ogg_stream_state os{};
    std::FILE* file = nullptr;
    int channels = 1;
    int sampleRate = 8000;
    bool headersWritten = false;

    bool open(const char* path, int sr, int ch)
    {
        channels = ch;
        sampleRate = sr;

        file = std::fopen(path, "wb");
        if (!file)
        {
            std::fprintf(stderr, "[ogg] fopen failed: %s\n", path);
            return false;
        }

        vorbis_info_init(&vi);
        if (vorbis_encode_init_vbr(&vi, channels, sampleRate, 0.4f) != 0)
        {
            std::fprintf(stderr, "[ogg] vorbis_encode_init_vbr failed: rate=%d ch=%d\n",
                         sampleRate, channels);
            std::fclose(file); file = nullptr;
            vorbis_info_clear(&vi);
            return false;
        }

        vorbis_comment_init(&vc);

        if (vorbis_analysis_init(&vd, &vi) != 0 ||
            vorbis_block_init(&vd, &vb) != 0)
        {
            std::fprintf(stderr, "[ogg] vorbis_analysis/block init failed\n");
            vorbis_comment_clear(&vc);
            vorbis_info_clear(&vi);
            std::fclose(file); file = nullptr;
            return false;
        }

        ogg_stream_init(&os, std::rand());
        ogg_packet header, comm, codec;
        vorbis_analysis_headerout(&vd, &vc, &header, &comm, &codec);
        ogg_stream_packetin(&os, &header);
        ogg_stream_packetin(&os, &comm);
        ogg_stream_packetin(&os, &codec);
        flushPages();
        headersWritten = true;

        return true;
    }

    void write(const int16_t* data, int frames)
    {
        if (!file || !headersWritten)
            return;

        // Convert int16 interleaved PCM -> float** buffers expected by libvorbis.
        float** bufs = vorbis_analysis_buffer(&vd, frames);
        for (int c = 0; c < channels; ++c)
        {
            float* dst = bufs[c];
            const int16_t* src = data + c;
            for (int i = 0; i < frames; ++i)
            {
                dst[i] = (float)src[i * channels] / 32768.0f;
            }
        }

        vorbis_analysis_wrote(&vd, frames);
        flushPages();
    }

    void close()
    {
        if (!file)
            return;

        if (headersWritten)
        {
            // Drain the analysis pipeline: signal end-of-stream.
            vorbis_analysis_wrote(&vd, 0);
            flushPages();
        }

        ogg_stream_clear(&os);
        vorbis_block_clear(&vb);
        vorbis_dsp_clear(&vd);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        std::fclose(file);
        file = nullptr;
    }

private:
    void flushPages()
    {
        while (vorbis_analysis_blockout(&vd, &vb) == 1)
        {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op))
            {
                ogg_stream_packetin(&os, &op);
                ogg_page og;
                while (ogg_stream_pageout(&os, &og))
                {
                    std::fwrite(og.header, 1, og.header_len, file);
                    std::fwrite(og.body, 1, og.body_len, file);
                }
            }
        }
    }
};

// ---- WavWriter public API ----

bool WavWriter::open(const std::string& path, int sampleRate, int channels)
{
    close();
    path_ = path;
    sampleRate_ = sampleRate;
    channels_ = channels;

    if (fmt_ == RecordFormat::OGG)
    {
        auto* enc = new WavWriterOggImpl{};
        if (!enc->open(path.c_str(), sampleRate, channels))
        {
            delete enc;
            return false;
        }
        ogg_ = enc;
        isOpen_ = true;
        return true;
    }

    // WAV path
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_)
        return false;
    dataBytes_ = 0;

    const uint32_t byteRate = (uint32_t)(sampleRate_ * channels_ * 2);
    const uint16_t blockAlign = (uint16_t)(channels_ * 2);

    std::fwrite("RIFF", 1, 4, f_);
    put32(f_, 36); // RIFF chunk size (patched on close)
    std::fwrite("WAVE", 1, 4, f_);
    std::fwrite("fmt ", 1, 4, f_);
    put32(f_, 16);                  // fmt chunk size
    put16(f_, 1);                   // PCM
    put16(f_, (uint16_t)channels_); // channels
    put32(f_, (uint32_t)sampleRate_);
    put32(f_, byteRate);
    put16(f_, blockAlign);
    put16(f_, 16); // bits per sample
    std::fwrite("data", 1, 4, f_);
    put32(f_, 0); // data chunk size (patched on close)
    isOpen_ = true;
    return true;
}

void WavWriter::write(const int16_t* data, int count)
{
    if (!isOpen_ || count <= 0)
        return;
    hasAudio_ = true;

    if (fmt_ == RecordFormat::OGG)
    {
        if (ogg_ && channels_ > 0)
            ogg_->write(data, count / channels_);
        return;
    }

    if (f_)
    {
        std::fwrite(data, sizeof(int16_t), (size_t)count, f_);
        dataBytes_ += (uint32_t)count * sizeof(int16_t);
    }
}

void WavWriter::close()
{
    if (!isOpen_)
        return;
    if (fmt_ == RecordFormat::OGG)
        closeOgg();
    else
        closeWav();
    isOpen_ = false;
    if (!hasAudio_ && !path_.empty())
    {
        std::remove(path_.c_str());
        path_.clear();
    }
    hasAudio_ = false;
}

void WavWriter::closeWav()
{
    if (!f_)
        return;
    std::fseek(f_, 4, SEEK_SET);
    put32(f_, 36 + dataBytes_);
    std::fseek(f_, 40, SEEK_SET);
    put32(f_, dataBytes_);
    std::fclose(f_);
    f_ = nullptr;
}

void WavWriter::closeOgg()
{
    if (ogg_)
    {
        ogg_->close();
        delete ogg_;
        ogg_ = nullptr;
    }
}
