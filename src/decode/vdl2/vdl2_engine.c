// VDL2 engine: dumpvdl2 demodulator/decoder core driven by AirScope's IQ.
// Only the AVLC/ACARS path is built (see vdl2_burst.c).
//
// IMPORTANT: dumpvdl2's burst decoder uses a large stack VLA (RS block table),
// and the SDR callback thread's stack is far too small for it. All demod work
// therefore runs on a dedicated worker thread with a large, explicit stack;
// vdl2_feed() only enqueues a copy of the IQ block.
#define _GNU_SOURCE
#include "decode/vdl2/vdl2.h"
#include "decode/vdl2/vdl2_burst.h"
#include "decode/libacars_lock.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "config.h"
#include "dumpvdl2.h"

#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/reassembly.h>

// Required by the dumpvdl2 core (demod.c reads Config.max_ppm). The barriers
// are only referenced by dumpvdl2's unused threaded input path.
dumpvdl2_config_t Config;
pthread_barrier_t demods_ready, samples_ready;

#define VDL2_MAX_CH      32
#define VDL2_CHUNK       2048        // complex samples per demod pass
#define VDL2_SINC_N      16
#define VDL2_QLEN        512         // queued IQ blocks (drop-newest if full)
#define VDL2_STACK_SIZE  (8u * 1024u * 1024u)
#define VDL2_RTAB        1024        // resampler filter table resolution

// Precomputed windowed-sinc resampler kernel h(x), x in [0, SINC_N].
// h is even, so negative offsets use |x|. Avoids per-tap sin/cos in the hot loop.
static float wtab[VDL2_SINC_N * VDL2_RTAB + 1];

static void resamp_table_init(void)
{
    for (int i = 0; i <= VDL2_SINC_N * VDL2_RTAB; ++i)
    {
        float x = (float)i / VDL2_RTAB;
        float pix = (float)M_PI * x;
        float s = (x == 0.0f) ? 1.0f : sinf(pix) / pix;
        float w = 0.42f + 0.5f * cosf((float)M_PI * x / VDL2_SINC_N)
                        + 0.08f * cosf(2.0f * (float)M_PI * x / VDL2_SINC_N);
        wtab[i] = s * w;
    }
}

static inline float wlookup(float x)
{
    float ax = fabsf(x);
    if (ax >= (float)VDL2_SINC_N)
        return 0.0f;
    float f = ax * VDL2_RTAB;
    int i = (int)f;
    float fr = f - (float)i;
    return wtab[i] + (wtab[i + 1] - wtab[i]) * fr;
}

// Catmull-Rom cubic interpolation between y1 (t=0) and y2 (t=1); y0/y3 are the
// neighbouring samples. Flat and cheap, and more than accurate enough when the
// resample ratio is close to 1 (the common live-SDR case).
static inline float catmull(float y0, float y1, float y2, float y3, float t)
{
    float a1 = 0.5f * (y2 - y0);
    float a2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float a3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((a3 * t + a2) * t + a1) * t + y1;
}

static struct
{
    int      inited;
    int      conjugate;
    int      oversample;
    double   fs;
    double   targetRate;
    unsigned nch;
    vdl2_channel_t* ch[VDL2_MAX_CH];

    // resampler state (worker thread only)
    float*   inBuf;
    long     inLen, inCap;
    double   phase;
    float    stage[VDL2_CHUNK * 2];
    int      stageN;

    vdl2_acars_cb cb;
    void*         user;
} g;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;   // start/stop vs feed
static la_reasm_ctx* g_reasm;
static volatile unsigned long g_frameCount = 0;

// Channel demodulator pool. dumpvdl2 runs one thread per channel; we fan each
// resampled chunk out to a pool of threads so multiple channels decode in
// parallel instead of serially on one core.
static int g_nWorkers = 0;
static pthread_t g_chTh[VDL2_MAX_CH];
static int g_chThValid[VDL2_MAX_CH];
static pthread_mutex_t g_chM = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_chWork = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_chDoneCv = PTHREAD_COND_INITIALIZER;
static int g_chGen = 0;          // bumped per dispatched chunk
static int g_chDone = 0;         // workers that finished the current chunk
static int g_chQuit = 0;
static const float* g_chBuf = NULL;
static int g_chBufN = 0;

// ---- IQ queue (producer: SDR callback; consumer: worker) -------------------
typedef struct { float* data; int n; } QItem;
static struct
{
    QItem    items[VDL2_QLEN];
    int      head, count;
    int      running;
    int      thValid;
    unsigned long dropped;
    pthread_t th;
} q;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcv = PTHREAD_COND_INITIALIZER;

// ---------------------------------------------------------------------------
// AVLC + ACARS (only the ACARS path of dumpvdl2's avlc.c)
// ---------------------------------------------------------------------------
static uint32_t dlc_addr(const uint8_t* buf)
{
    return reverse((buf[0] >> 1) | (buf[1] << 6) | (buf[2] << 13) |
                   ((buf[3] & 0xfe) << 20), 28) & ONES(28);
}

void airscope_vdl2_frame(vdl2_frame_t const* f)
{
    if (!f || f->len < 11)
        return;
    uint8_t const* buf = f->data;
    uint32_t len = f->len;
    if (crc16_ccitt((uint8_t*)buf, len, 0xFFFFu) != 0xF0B8u)
        return;                              // bad FCS
    ++g_frameCount;
    len -= 2;
    if (len < 11)
        return;
    uint32_t src = dlc_addr(buf + 4);
    uint8_t lcf = buf[8];
    if ((lcf & 0x1) != 0)
        return;                              // only I-frames carry ACARS
    const uint8_t* p = buf + 9;
    uint32_t plen = len - 9;
    if (!(plen > 3 && p[0] == 0xff && p[1] == 0xff && p[2] == 0x01))
        return;

    // libacars uses process-wide state (and a shared reassembly context); the
    // channel workers call this concurrently, so serialize it. Messages are
    // infrequent, so this costs nothing noticeable.
    airscope_libacars_lock();
    if (!g_reasm)
        g_reasm = la_reasm_ctx_new();
    la_msg_dir dir = ((src >> 24) & 0x7) == 1 ? LA_MSG_DIR_AIR2GND
                                              : LA_MSG_DIR_GND2AIR;
    struct timeval tv = f->ts;
    la_proto_node* root =
        la_acars_parse_and_reassemble(p + 3, (int)(plen - 3), dir, g_reasm, tv);
    if (!root)
    {
        airscope_libacars_unlock();
        return;
    }
    la_proto_node* an = la_proto_tree_find_acars(root);
    vdl2_acars_t o;
    memset(&o, 0, sizeof(o));
    int have = 0;
    if (an)
    {
        la_acars_msg* m = (la_acars_msg*)an->data;
        if (!m->err)
        {
            o.mode = m->mode;
            o.ack = m->ack;
            o.blockId = m->block_id;
            snprintf(o.reg, sizeof(o.reg), "%s", m->reg);
            snprintf(o.label, sizeof(o.label), "%s", m->label);
            snprintf(o.msgNum, sizeof(o.msgNum), "%s", m->msg_num);
            snprintf(o.flight, sizeof(o.flight), "%s", m->flight_id);
            snprintf(o.txt, sizeof(o.txt), "%s", m->txt ? m->txt : "");
            o.freqHz = f->freq;
            o.timeSec = (double)f->ts.tv_sec + (double)f->ts.tv_usec / 1e6;
            o.downlink = (dir == LA_MSG_DIR_AIR2GND) ? 1 : 0;
            have = 1;
        }
    }
    la_proto_tree_destroy(root);
    airscope_libacars_unlock();

    // Deliver outside the libacars lock: the sink writes to a shared message
    // log and must not stall other channel workers' parsing.
    if (have && g.cb)
        g.cb(&o, g.user);
}

// ---------------------------------------------------------------------------
// Worker: resample + demodulate
// ---------------------------------------------------------------------------
void vdl2_set_acars_cb(vdl2_acars_cb cb, void* user) { g.cb = cb; g.user = user; }
int vdl2_running(void) { return q.running; }
unsigned long vdl2_frame_count(void) { return g_frameCount; }
unsigned long vdl2_dropped_count(void) { return q.dropped; }

static void flush_stage(void)
{
    if (g.stageN <= 0)
        return;
    if (g_nWorkers > 1)
    {
        pthread_mutex_lock(&g_chM);
        g_chBuf = g.stage;
        g_chBufN = g.stageN * 2;
        g_chDone = 0;
        ++g_chGen;
        pthread_cond_broadcast(&g_chWork);
        while (g_chDone < g_nWorkers)
            pthread_cond_wait(&g_chDoneCv, &g_chM);
        pthread_mutex_unlock(&g_chM);
    }
    else
    {
        for (unsigned i = 0; i < g.nch; ++i)
            demod_process(g.ch[i], g.stage, (uint32_t)g.stageN * 2);
    }
    g.stageN = 0;
}

static inline void emit(float re, float im)
{
    g.stage[g.stageN * 2]     = re;
    g.stage[g.stageN * 2 + 1] = im;
    if (++g.stageN == VDL2_CHUNK)
        flush_stage();
}

static void process_block(const float* iq, int nComplex)
{
    const double step = g.fs / g.targetRate;
    const int identity = fabs(step - 1.0) < 1e-4;
    const float sgn = g.conjugate ? -1.0f : 1.0f;

    if (identity)
    {
        for (int i = 0; i < nComplex; ++i)
            emit(iq[i * 2], iq[i * 2 + 1] * sgn);
        flush_stage();
        return;
    }

    long need = g.inLen + (long)nComplex * 2;
    if (need > g.inCap)
    {
        g.inCap = need * 2 + 4096;
        g.inBuf = (float*)realloc(g.inBuf, (size_t)g.inCap * sizeof(float));
    }
    for (int i = 0; i < nComplex; ++i)
    {
        g.inBuf[g.inLen + i * 2]     = iq[i * 2];
        g.inBuf[g.inLen + i * 2 + 1] = iq[i * 2 + 1] * sgn;
    }
    g.inLen += (long)nComplex * 2;

    const long nIn = g.inLen / 2;

    // Near-unity ratio (real SDRs): cubic interpolation is ~8x cheaper than the
    // 32-tap windowed-sinc kernel and its droop across a +/-5 kHz channel is
    // negligible. Large ratios (e.g. 48 kHz WAV upsampling) use the full kernel.
    if (fabs(step - 1.0) < 0.05)
    {
        while (g.phase + 2.0 < (double)nIn)
        {
            long base = (long)floor(g.phase);
            long i0 = base - 1 < 0 ? 0 : base - 1;
            float t = (float)(g.phase - (double)base);
            const float* p0 = &g.inBuf[i0 * 2];
            const float* p1 = &g.inBuf[base * 2];
            const float* p2 = &g.inBuf[(base + 1) * 2];
            const float* p3 = &g.inBuf[(base + 2) * 2];
            emit(catmull(p0[0], p1[0], p2[0], p3[0], t),
                 catmull(p0[1], p1[1], p2[1], p3[1], t));
            g.phase += step;
        }
        flush_stage();
        long consumed = (long)g.phase - (VDL2_SINC_N + 1);
        if (consumed > 0)
        {
            memmove(g.inBuf, g.inBuf + consumed * 2,
                    (size_t)(g.inLen - consumed * 2) * sizeof(float));
            g.inLen -= consumed * 2;
            g.phase -= (double)consumed;
        }
        return;
    }

    while (g.phase + VDL2_SINC_N + 1 < (double)nIn)
    {
        long base = (long)floor(g.phase);
        float accI = 0, accQ = 0, wsum = 0;
        for (long i = base - VDL2_SINC_N + 1; i <= base + VDL2_SINC_N; ++i)
        {
            long ii = i < 0 ? 0 : i;
            float w = wlookup((float)(g.phase - (double)i));
            accI += g.inBuf[ii * 2] * w;
            accQ += g.inBuf[ii * 2 + 1] * w;
            wsum += w;
        }
        emit(accI / wsum, accQ / wsum);
        g.phase += step;
    }
    flush_stage();

    long consumed = (long)g.phase - (VDL2_SINC_N + 1);
    if (consumed > 0)
    {
        memmove(g.inBuf, g.inBuf + consumed * 2,
                (size_t)(g.inLen - consumed * 2) * sizeof(float));
        g.inLen -= consumed * 2;
        g.phase -= (double)consumed;
    }
}

static void queue_reset_state()
{
    g.phase = 0.0;
    g.inLen = 0;
    g.stageN = 0;
    if (g.inBuf) { free(g.inBuf); g.inBuf = NULL; g.inCap = 0; }
}

static void* vdl2_worker(void* arg)
{
    (void)arg;
    for (;;)
    {
        pthread_mutex_lock(&g_qm);
        while (q.count == 0 && q.running)
            pthread_cond_wait(&g_qcv, &g_qm);
        if (q.count == 0 && !q.running)
        {
            pthread_mutex_unlock(&g_qm);
            break;
        }
        QItem it = q.items[q.head];
        q.head = (q.head + 1) % VDL2_QLEN;
        --q.count;
        pthread_mutex_unlock(&g_qm);

        process_block(it.data, it.n);
        free(it.data);
    }
    return NULL;
}

// Channel demod workers: each waits for a freshly resampled chunk, processes
// its share of the channels, then reports completion to the resampler thread.
static void* vdl2_chworker(void* arg)
{
    int id = (int)(intptr_t)arg;
    int seen = 0;
    for (;;)
    {
        pthread_mutex_lock(&g_chM);
        while (g_chGen == seen && !g_chQuit)
            pthread_cond_wait(&g_chWork, &g_chM);
        if (g_chQuit && g_chGen == seen)
        {
            pthread_mutex_unlock(&g_chM);
            break;
        }
        seen = g_chGen;
        const float* buf = g_chBuf;
        int n = g_chBufN;
        unsigned nch = g.nch;
        int nw = g_nWorkers;
        pthread_mutex_unlock(&g_chM);

        for (unsigned i = (unsigned)id; i < nch; i += (unsigned)nw)
            demod_process(g.ch[i], buf, (uint32_t)n);

        pthread_mutex_lock(&g_chM);
        ++g_chDone;
        pthread_cond_signal(&g_chDoneCv);
        pthread_mutex_unlock(&g_chM);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public feed / lifecycle
// ---------------------------------------------------------------------------
void vdl2_feed(const float* iq, int nComplex)
{
    if (nComplex <= 0)
        return;
    float* copy = (float*)malloc((size_t)nComplex * 2 * sizeof(float));
    if (!copy)
        return;
    memcpy(copy, iq, (size_t)nComplex * 2 * sizeof(float));

    pthread_mutex_lock(&g_qm);
    if (!q.running || q.count == VDL2_QLEN)
    {
        ++q.dropped;
        pthread_mutex_unlock(&g_qm);
        free(copy);
        return;
    }
    int idx = (q.head + q.count) % VDL2_QLEN;
    q.items[idx].data = copy;
    q.items[idx].n = nComplex;
    ++q.count;
    pthread_cond_signal(&g_qcv);
    pthread_mutex_unlock(&g_qm);
}

int vdl2_start(double sourceRateHz, double centerHz, const double* freqsHz,
               int nChannels, int conjugate)
{
    pthread_mutex_lock(&g_mtx);
    if (q.running)
    {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    if (nChannels <= 0 || nChannels > VDL2_MAX_CH || !freqsHz)
    {
        pthread_mutex_unlock(&g_mtx);
        return -2;
    }
    if (sourceRateHz < 8000.0)
    {
        pthread_mutex_unlock(&g_mtx);
        return -3;
    }

    memset(&Config, 0, sizeof(Config));
    Config.max_ppm = 0;

    int oversample = (int)lround(sourceRateHz / 105000.0);
    if (oversample < 1) oversample = 1;
    g.oversample = oversample;
    g.targetRate = 105000.0 * oversample;
    g.fs = sourceRateHz;
    g.conjugate = conjugate;
    g.nch = (unsigned)nChannels;
    queue_reset_state();
    g_frameCount = 0;

    input_lpf_init((uint32_t)g.targetRate);
    if (!g.inited)
    {
        sincosf_lut_init();
        demod_sync_init();
        rs_init();
        resamp_table_init();
        g.inited = 1;
    }

    for (unsigned i = 0; i < g.nch; ++i)
    {
        g.ch[i] = vdl2_channel_init((uint32_t)llround(centerHz),
                                    (uint32_t)llround(freqsHz[i]),
                                    (uint32_t)g.targetRate, oversample);
        if (!g.ch[i])
        {
            for (unsigned j = 0; j < i; ++j)
            {
                bitstream_destroy(g.ch[j]->bs);
                bitstream_destroy(g.ch[j]->frame_bs);
                free(g.ch[j]);
                g.ch[j] = NULL;
            }
            g.nch = 0;
            pthread_mutex_unlock(&g_mtx);
            return -4;
        }
    }

    // Channel demod pool: one worker per channel (capped at the CPU count).
    // Falls back to inline processing on the resampler thread if creation fails.
    long ncpu;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ncpu = (long)si.dwNumberOfProcessors;
#else
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (ncpu < 1) ncpu = 4;
    int nw = (int)g.nch;
    if (nw > (int)ncpu) nw = (int)ncpu;
    if (nw < 1) nw = 1;
    g_nWorkers = nw;
    g_chQuit = 0;
    g_chGen = 0;
    g_chDone = 0;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, VDL2_STACK_SIZE);

    for (int k = 0; k < g_nWorkers; ++k)
    {
        g_chThValid[k] = 0;
        if (pthread_create(&g_chTh[k], &at, vdl2_chworker, (void*)(intptr_t)k) == 0)
            g_chThValid[k] = 1;
        else
        {
            g_nWorkers = k;
            break;
        }
    }

    // Resampler thread (also owns the input queue).
    q.head = 0;
    q.count = 0;
    q.dropped = 0;
    q.running = 1;
    q.thValid = 0;
    if (pthread_create(&q.th, &at, vdl2_worker, NULL) != 0)
    {
        q.running = 0;
        pthread_mutex_lock(&g_chM);
        g_chQuit = 1;
        pthread_cond_broadcast(&g_chWork);
        pthread_mutex_unlock(&g_chM);
        for (int k = 0; k < g_nWorkers; ++k)
            if (g_chThValid[k]) { pthread_join(g_chTh[k], NULL); g_chThValid[k] = 0; }
        g_nWorkers = 0;
        pthread_attr_destroy(&at);
        for (unsigned i = 0; i < g.nch; ++i) { free(g.ch[i]); g.ch[i] = NULL; }
        g.nch = 0;
        pthread_mutex_unlock(&g_mtx);
        return -5;
    }
    q.thValid = 1;
    pthread_attr_destroy(&at);

    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void vdl2_stop(void)
{
    pthread_mutex_lock(&g_mtx);
    if (!q.running && !q.thValid && g.nch == 0 && g_nWorkers == 0)
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    // Stop the resampler thread first: after it joins no further chunks are
    // dispatched, so the channel pool can then be shut down safely.
    pthread_mutex_lock(&g_qm);
    q.running = 0;
    pthread_cond_broadcast(&g_qcv);
    pthread_mutex_unlock(&g_qm);
    if (q.thValid)
    {
        pthread_join(q.th, NULL);
        q.thValid = 0;
    }

    if (g_nWorkers > 0)
    {
        pthread_mutex_lock(&g_chM);
        g_chQuit = 1;
        pthread_cond_broadcast(&g_chWork);
        pthread_mutex_unlock(&g_chM);
        for (int k = 0; k < g_nWorkers; ++k)
            if (g_chThValid[k]) { pthread_join(g_chTh[k], NULL); g_chThValid[k] = 0; }
        g_nWorkers = 0;
    }

    // Free any queued blocks that were never processed.
    pthread_mutex_lock(&g_qm);
    while (q.count > 0)
    {
        free(q.items[q.head].data);
        q.head = (q.head + 1) % VDL2_QLEN;
        --q.count;
    }
    pthread_mutex_unlock(&g_qm);

    for (unsigned i = 0; i < g.nch; ++i)
    {
        if (!g.ch[i]) continue;
        bitstream_destroy(g.ch[i]->bs);
        bitstream_destroy(g.ch[i]->frame_bs);
        free(g.ch[i]);
        g.ch[i] = NULL;
    }
    g.nch = 0;
    pthread_mutex_unlock(&g_mtx);
}
