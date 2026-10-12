/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "husk-tl-bionic.h"
#include "husk-tl-cocos.h"

#define NBUF 3
#define FRAMES_PER_BUF 1024
#define RING_FRAMES 8192

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    AudioQueueRef q;
    AudioQueueBufferRef bufs[NBUF];
    int rate, channels;
    int16_t *ring;
    size_t head, count;           /* frames */
    bool paused, failed;
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* The audio thread asks for samples; an empty ring is silence, never a wait. */
static void on_buffer(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
    (void)user;
    int16_t *out = b->mAudioData;
    size_t want = FRAMES_PER_BUF, ch = (size_t)A.channels;
    pthread_mutex_lock(&A.mu);
    size_t take = A.paused ? 0 : (A.count < want ? A.count : want);
    for (size_t i = 0; i < take; i++)
        for (size_t c = 0; c < ch; c++) out[i * ch + c] = A.ring[((A.head + i) % RING_FRAMES) * ch + c];
    A.head = (A.head + take) % RING_FRAMES;
    A.count -= take;
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
    if (take < want) memset(out + take * ch, 0, (want - take) * ch * sizeof(int16_t));
    b->mAudioDataByteSize = (UInt32)(want * ch * sizeof(int16_t));
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static bool open_queue(int rate, int channels)
{
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBitsPerChannel = 16; f.mChannelsPerFrame = (UInt32)channels; f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = (UInt32)(channels * 2);
    OSStatus st = AudioQueueNewOutput(&f, on_buffer, NULL, NULL, NULL, 0, &A.q);
    if (st != noErr) { tl_log_line("audio: AudioQueueNewOutput failed (%d)", (int)st); return false; }
    A.ring = calloc((size_t)RING_FRAMES * (size_t)channels, sizeof(int16_t));
    A.rate = rate; A.channels = channels;
    for (int i = 0; i < NBUF; i++) {
        AudioQueueAllocateBuffer(A.q, FRAMES_PER_BUF * (UInt32)channels * 2, &A.bufs[i]);
        memset(A.bufs[i]->mAudioData, 0, FRAMES_PER_BUF * (size_t)channels * 2);
        A.bufs[i]->mAudioDataByteSize = FRAMES_PER_BUF * (UInt32)channels * 2;
        AudioQueueEnqueueBuffer(A.q, A.bufs[i], 0, NULL);
    }
    st = AudioQueueStart(A.q, NULL);
    if (st != noErr) { tl_log_line("audio: AudioQueueStart failed (%d)", (int)st); return false; }
    tl_log_line("audio: output running, %d Hz, %d channel(s)", rate, channels);
    return true;
}

static void sleep_for(int frames, int rate)
{
    struct timespec ts = { 0, (long)((double)frames * 1e9 / rate) };
    nanosleep(&ts, NULL);
}

static void ring_write(const int16_t *samples, int frames, int channels, int rate);

/*
 * The mixer's write, from any engine and any thread: whatever rate and channel count it plays at, the host queue is opened once at 48 kHz stereo and every
 * write is converted to that here -- channels first (mono is doubled, more than two keep the front pair), then a linear-interpolation resampler that keeps
 * its position and last sample between calls, so there is no click at a block boundary. Before this the queue took the rate of whichever write came first and
 * played every later one at that rate (a 24 kHz Unity/FMOD mix, a 44.1 kHz video soundtrack and a 48 kHz AAudio stream could not all be right), which is the
 * high, fast voices of Silksong.
 */
#define OUT_RATE 48000
static void host_write(const int16_t *samples, int frames, int channels, int rate)
{
    if (!samples || frames <= 0 || channels <= 0) return;
    if (rate < 4000 || rate > 192000) rate = OUT_RATE;
    if (rate == OUT_RATE && channels == 2) { ring_write(samples, frames, 2, OUT_RATE); return; }
    static __thread double pos;                    /* where the next output frame falls in the source: -1 is the previous block's last frame */
    static __thread int16_t prev[2];
    static __thread int last_rate;
    if (last_rate != rate) { last_rate = rate; pos = 0; prev[0] = samples[0]; prev[1] = channels > 1 ? samples[1] : samples[0]; }
    const double step = (double)rate / OUT_RATE;
    size_t cap = (size_t)((double)frames / step) + 8, n = 0;
    int16_t *out = malloc(cap * 2 * sizeof(int16_t));
    if (!out) return;
#define SRC(i, c) ((i) < 0 ? prev[c] : samples[(size_t)(i) * (size_t)channels + ((c) < channels ? (size_t)(c) : 0)])
    while (n < cap) {
        long i = (long)pos; if (pos < 0 && (double)i != pos) i--;      /* floor */
        if (i + 1 >= frames) break;
        double f = pos - (double)i;
        for (int c = 0; c < 2; c++) {
            double a = SRC(i, c), b = SRC(i + 1, c);
            out[n * 2 + (size_t)c] = (int16_t)(a + (b - a) * f);
        }
        n++;
        pos += step;
    }
#undef SRC
    pos -= frames;
    prev[0] = samples[(size_t)(frames - 1) * (size_t)channels];
    prev[1] = channels > 1 ? samples[(size_t)(frames - 1) * (size_t)channels + 1] : prev[0];
    if (n) ring_write(out, (int)n, 2, OUT_RATE);
    free(out);
}

/* Copy into the ring, blocking while it is full (that is the pacing). */
static void ring_write(const int16_t *samples, int frames, int channels, int rate)
{
    pthread_mutex_lock(&A.mu);
    if (!A.q && !A.failed) { if (!open_queue(OUT_RATE, 2)) A.failed = true; }
    if (A.failed || channels != A.channels) { pthread_mutex_unlock(&A.mu); sleep_for(frames, rate); return; }
    int done = 0;
    while (done < frames) {
        while (A.count >= RING_FRAMES || A.paused) pthread_cond_wait(&A.cv, &A.mu);
        size_t room = RING_FRAMES - A.count, n = (size_t)(frames - done) < room ? (size_t)(frames - done) : room;
        size_t tail = (A.head + A.count) % RING_FRAMES;
        for (size_t i = 0; i < n; i++)
            memcpy(&A.ring[((tail + i) % RING_FRAMES) * (size_t)channels], &samples[(size_t)(done + (int)i) * (size_t)channels], (size_t)channels * sizeof(int16_t));
        A.count += n;
        done += (int)n;
    }
    pthread_mutex_unlock(&A.mu);
}

/* TL_AUDIO_MUTE: pace like a device but make no sound (for test runs on a Mac). TL_AUDIO_STATS=1 also logs, about once a second,
 * how loud the game's output was, so a silent run still shows whether the game is making sound. */
static void muted_write(const int16_t *samples, int frames, int channels, int rate)
{
    static int stats = -1, seen, peak;
    static long loud, total;
    if (stats < 0) stats = getenv("TL_AUDIO_STATS") != NULL;
    if (stats) {
        for (int i = 0; i < frames * channels; i++) {
            int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak) peak = v;
            if (v > 64) loud++;
        }
        total += frames * channels;
        if ((seen += frames) >= rate) {
            tl_log_line("audio: %d Hz x %d, peak %d, %.0f%% of samples audible", rate, channels, peak, 100.0 * loud / (total ? total : 1));
            seen = 0; peak = 0; loud = 0; total = 0;
        }
    }
    sleep_for(frames, rate);
}

void tl_audio_install(void) { tl_cocos_audio_hook = getenv("TL_AUDIO_MUTE") ? muted_write : host_write; }

void tl_audio_set_paused(bool paused)
{
    pthread_mutex_lock(&A.mu);
    A.paused = paused;
    if (A.q) { if (paused) AudioQueuePause(A.q); else AudioQueueStart(A.q, NULL); }
    pthread_cond_broadcast(&A.cv);
    pthread_mutex_unlock(&A.mu);
}
