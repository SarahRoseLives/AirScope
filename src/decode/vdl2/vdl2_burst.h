// Shared type for the VDL2 burst decoder (vdl2_burst.c) and its consumer.
// vdl2_burst.c is derived from dumpvdl2 src/decode.c (GPLv3).
#pragma once

#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t freq;
    int idx;
    uint8_t const *data;
    uint32_t len;
    uint32_t num_fec_corrections;
    float frame_pwr_dbfs;
    float nf_pwr_dbfs;
    float ppm_error;
    struct timeval ts;
} vdl2_frame_t;

// Implemented by the VDL2 engine; called from the demod thread for each
// FEC-correct frame.
void airscope_vdl2_frame(vdl2_frame_t const *f);

#ifdef __cplusplus
}
#endif
