// VDL Mode 2 receiver engine for AirScope.
//
// Wraps the dumpvdl2 demodulator/decoder core (vendored in third_party/dumpvdl2)
// to decode ACARS-over-VDL2 from a wideband IQ stream. AirScope builds only the
// AVLC/ACARS path; decoded ACARS messages are handed to a C callback.
//
// The engine is process-wide (the vendored demodulator uses global state), so
// only one instance may run at a time.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     mode;
    char     reg[8];
    char     ack;
    char     label[3];
    char     blockId;
    char     msgNum[4];
    char     flight[7];
    char     txt[512];
    uint32_t freqHz;
    double   timeSec;
    uint8_t  downlink;   // 1 = air->ground, 0 = ground->ai
} vdl2_acars_t;

typedef void (*vdl2_acars_cb)(const vdl2_acars_t* msg, void* user);

// Register the ACARS sink. Call before vdl2_start(). `user` is passed through.
void vdl2_set_acars_cb(vdl2_acars_cb cb, void* user);

// Start decoding. `freqsHz` are the absolute channel frequencies (Hz).
// `conjugate` inverts the Q axis (some recordings have an inverted spectrum).
// Returns 0 on success.
int  vdl2_start(double sourceRateHz, double centerHz, const double* freqsHz,
                int nChannels, int conjugate);

void vdl2_stop(void);

// Feed interleaved float IQ (re/im) from the SDR thread. Internally resampled
// to a demodulator-friendly rate and blocked as needed.
void vdl2_feed(const float* iq, int nComplex);

int  vdl2_running(void);

// Number of good-FCS frames demodulated since start (diagnostic).
unsigned long vdl2_frame_count(void);

// Number of IQ blocks dropped because the processing queue was full (diagnostic).
unsigned long vdl2_dropped_count(void);

#ifdef __cplusplus
}
#endif
