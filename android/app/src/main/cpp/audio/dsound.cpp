// DirectSound 8 on AAudio.
//
// Secondary buffers live in guest memory (the game writes PCM there between
// Lock/Unlock). One AAudio output stream pulls from every playing buffer in its
// data callback, resampling with linear interpolation, applying volume/pan,
// and advancing each buffer's play cursor in real time. GetCurrentPosition
// reports that cursor, which is what EA's streaming mixer writes ahead of.
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/guest_heap.h"
#include "hle/com.h"
#include "hle/hle.h"

using hle::arg;
using hle::ret;

namespace {
constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057, DSERR_UNSUPPORTED = 0x80004001,
                   DSERR_NOINTERFACE = 0x80004002;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1, DSBPLAY_LOOPING = 0x1;
constexpr int kOutRate = 48000;

struct DSound : com::Object {};

struct Buffer : com::Object {
    bool primary = false;
    uint32_t flags = 0;
    uint32_t mem = 0, size = 0;
    uint16_t channels = 2, bits = 16;
    uint32_t rate = 22050;
    uint32_t freq = 0;            // SetFrequency override (0 = format rate)
    int32_t volume = 0, pan = 0;  // hundredths of dB, -10000..10000
    bool playing = false, looping = false;
    double pos = 0.0;             // play cursor in frames (fractional for resampling)
    uint32_t block() const { return (uint32_t)channels * bits / 8; }
    uint32_t frames() const { return block() ? size / block() : 0; }
    ~Buffer() override {
        if (mem) gheap::free(mem);
    }
};

std::mutex g_mu;  // guards g_buffers and buffer playback fields
std::vector<Buffer*> g_buffers;
AAudioStream* g_stream = nullptr;

float sample(const Buffer* b, uint32_t frame, int ch) {
    uint32_t c = std::min<uint32_t>((uint32_t)ch, b->channels - 1u);
    uint32_t off = frame * b->block() + c * (b->bits / 8);
    if (b->bits == 16) return (float)(int16_t)mem::rd16(b->mem + off) / 32768.f;
    return ((float)mem::rd8(b->mem + off) - 128.f) / 128.f;
}

aaudio_data_callback_result_t mix(AAudioStream*, void*, void* audio, int32_t nframes) {
    float* out = (float*)audio;
    memset(out, 0, sizeof(float) * 2 * (size_t)nframes);
    std::lock_guard<std::mutex> lk(g_mu);
    for (Buffer* b : g_buffers) {
        if (!b->playing || b->primary || !b->mem || b->frames() == 0) continue;
        const double step = (double)(b->freq ? b->freq : b->rate) / kOutRate;
        const float gain = b->volume <= -10000 ? 0.f : std::pow(10.f, (float)b->volume / 2000.f);
        const float pan = (float)b->pan / 10000.f;
        const float gl = gain * (pan > 0 ? 1.f - pan : 1.f), gr = gain * (pan < 0 ? 1.f + pan : 1.f);
        const uint32_t total = b->frames();
        for (int32_t i = 0; i < nframes; ++i) {
            uint32_t f0 = (uint32_t)b->pos;
            uint32_t f1 = f0 + 1 < total ? f0 + 1 : (b->looping ? 0 : f0);
            float t = (float)(b->pos - (double)f0);
            float l = sample(b, f0, 0) * (1 - t) + sample(b, f1, 0) * t;
            float r = sample(b, f0, 1) * (1 - t) + sample(b, f1, 1) * t;
            out[i * 2] += l * gl;
            out[i * 2 + 1] += r * gr;
            b->pos += step;
            if (b->pos >= total) {
                if (b->looping) {
                    b->pos -= total;
                } else {
                    b->pos = 0;
                    b->playing = false;
                    break;
                }
            }
        }
    }
    for (int32_t i = 0; i < nframes * 2; ++i) out[i] = std::clamp(out[i], -1.f, 1.f);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void start_output() {
    if (g_stream) return;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, kOutRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, mix, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        g_stream = nullptr;
        return;
    }
    AAudioStream_requestStart(g_stream);
    LOGI("AAudio output started: %d Hz, burst %d frames", AAudioStream_getSampleRate(g_stream),
         AAudioStream_getFramesPerBurst(g_stream));
}

uint32_t dsound_vtable();
uint32_t buffer_vtable();

Buffer* buf(cpu::Cpu& c) { return static_cast<Buffer*>(com::from_guest(arg(c, 0))); }

// ---------------------------------------------------------------- IDirectSound
void ds_CreateSoundBuffer(cpu::Cpu& c) {
    uint32_t desc = arg(c, 1), out = arg(c, 2);
    uint32_t flags = mem::rd32(desc + 4), bytes = mem::rd32(desc + 8), wfx = mem::rd32(desc + 16);
    Buffer* b = new Buffer();
    b->flags = flags;
    b->primary = flags & DSBCAPS_PRIMARYBUFFER;
    if (wfx) {
        b->channels = mem::rd16(wfx + 2);
        b->rate = mem::rd32(wfx + 4);
        b->bits = mem::rd16(wfx + 14);
    }
    if (!b->primary) {
        b->size = bytes;
        b->mem = gheap::alloc(bytes, true);
    }
    com::wrap(b, buffer_vtable());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buffers.push_back(b);
    }
    LOGI("DirectSound buffer: %s %u bytes, %u Hz %u-bit %uch, flags %x", b->primary ? "primary" : "secondary",
         bytes, b->rate, b->bits, b->channels, flags);
    mem::wr32(out, b->guest);
    start_output();
    ret(c, DS_OK);
}
void ds_GetCaps(cpu::Cpu& c) {
    uint32_t p = arg(c, 1);
    uint32_t sz = mem::rd32(p);
    memset(mem::ptr(p + 4), 0, sz > 4 ? sz - 4 : 0);
    mem::wr32(p + 4, 0x00000F5F);  // DSCAPS_*: primary/secondary mono/stereo/8/16 bit, continuous rate
    mem::wr32(p + 8, 100);
    mem::wr32(p + 12, 200000);
    mem::wr32(p + 20, 64);         // max hw mixing buffers
    ret(c, DS_OK);
}
void ds_ok(cpu::Cpu& c) { ret(c, DS_OK); }
void ds_GetSpeakerConfig(cpu::Cpu& c) {
    mem::wr32(arg(c, 1), 4);  // DSSPEAKER_STEREO
    ret(c, DS_OK);
}
void ds_DuplicateSoundBuffer(cpu::Cpu& c) {
    Buffer* src = static_cast<Buffer*>(com::from_guest(arg(c, 1)));
    if (!src) return ret(c, DSERR_INVALIDPARAM);
    Buffer* b = new Buffer();
    b->flags = src->flags;
    b->channels = src->channels;
    b->bits = src->bits;
    b->rate = src->rate;
    b->size = src->size;
    b->mem = gheap::alloc(src->size, false);
    memcpy(mem::ptr(b->mem), mem::ptr(src->mem), src->size);
    com::wrap(b, buffer_vtable());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buffers.push_back(b);
    }
    mem::wr32(arg(c, 2), b->guest);
    ret(c, DS_OK);
}

const com::Method kDSoundMethods[] = {
    {"IDirectSound8::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirectSound8::AddRef", 4, com::AddRef, 0},
    {"IDirectSound8::Release", 4, com::Release, 0},
    {"IDirectSound8::CreateSoundBuffer", 16, ds_CreateSoundBuffer, 0},
    {"IDirectSound8::GetCaps", 8, ds_GetCaps, 0},
    {"IDirectSound8::DuplicateSoundBuffer", 12, ds_DuplicateSoundBuffer, 0},
    {"IDirectSound8::SetCooperativeLevel", 12, ds_ok, 0},
    {"IDirectSound8::Compact", 4, ds_ok, 0},
    {"IDirectSound8::GetSpeakerConfig", 8, ds_GetSpeakerConfig, 0},
    {"IDirectSound8::SetSpeakerConfig", 8, ds_ok, 0},
    {"IDirectSound8::Initialize", 8, ds_ok, 0},
    {"IDirectSound8::VerifyCertification", 8, ds_ok, 0},
};

// ---------------------------------------------------------------- IDirectSoundBuffer
void b_Release(cpu::Cpu& c) {
    Buffer* b = buf(c);
    if (!b) return ret(c, 0);
    if (b->refs.load() == 1) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buffers.erase(std::remove(g_buffers.begin(), g_buffers.end(), b), g_buffers.end());
    }
    ret(c, com::release(b));
}
void b_GetCaps(cpu::Cpu& c) {
    Buffer* b = buf(c);
    uint32_t p = arg(c, 1);
    mem::wr32(p + 4, b->flags | 0x8);  // + LOCSOFTWARE
    mem::wr32(p + 8, b->size);
    mem::wr32(p + 12, 0);
    mem::wr32(p + 16, 0);
    ret(c, DS_OK);
}
void b_GetCurrentPosition(cpu::Cpu& c) {
    Buffer* b = buf(c);
    uint32_t play, write;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        play = (uint32_t)b->pos * b->block();
    }
    // Write cursor a little ahead of the play cursor, as real hardware reports.
    write = b->size ? (play + std::min<uint32_t>(b->size / 8, 4096)) % b->size : 0;
    if (arg(c, 1)) mem::wr32(arg(c, 1), play);
    if (arg(c, 2)) mem::wr32(arg(c, 2), write);
    ret(c, DS_OK);
}
void b_GetFormat(cpu::Cpu& c) {
    Buffer* b = buf(c);
    uint32_t p = arg(c, 1), cap = arg(c, 2), written = arg(c, 3);
    if (p && cap >= 16) {
        mem::wr16(p + 0, 1);
        mem::wr16(p + 2, b->channels);
        mem::wr32(p + 4, b->rate);
        mem::wr32(p + 8, b->rate * b->block());
        mem::wr16(p + 12, (uint16_t)b->block());
        mem::wr16(p + 14, b->bits);
        if (cap >= 18) mem::wr16(p + 16, 0);
    }
    if (written) mem::wr32(written, 18);
    ret(c, DS_OK);
}
void b_GetVolume(cpu::Cpu& c) {
    mem::wr32(arg(c, 1), (uint32_t)buf(c)->volume);
    ret(c, DS_OK);
}
void b_GetPan(cpu::Cpu& c) {
    mem::wr32(arg(c, 1), (uint32_t)buf(c)->pan);
    ret(c, DS_OK);
}
void b_GetFrequency(cpu::Cpu& c) {
    Buffer* b = buf(c);
    mem::wr32(arg(c, 1), b->freq ? b->freq : b->rate);
    ret(c, DS_OK);
}
void b_GetStatus(cpu::Cpu& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> lk(g_mu);
    mem::wr32(arg(c, 1), (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u));
    ret(c, DS_OK);
}
void b_Lock(cpu::Cpu& c) {
    Buffer* b = buf(c);
    uint32_t off = arg(c, 1), bytes = arg(c, 2), p1 = arg(c, 3), n1 = arg(c, 4), p2 = arg(c, 5), n2 = arg(c, 6),
             flags = arg(c, 7);
    if (b->primary || !b->size) return ret(c, DSERR_INVALIDPARAM);
    if (flags & 2) bytes = b->size;  // DSBLOCK_ENTIREBUFFER
    if (flags & 1) {                 // DSBLOCK_FROMWRITECURSOR
        std::lock_guard<std::mutex> lk(g_mu);
        off = ((uint32_t)b->pos * b->block() + std::min<uint32_t>(b->size / 8, 4096)) % b->size;
    }
    off %= b->size;
    bytes = std::min(bytes, b->size);
    uint32_t first = std::min(bytes, b->size - off);
    mem::wr32(p1, b->mem + off);
    mem::wr32(n1, first);
    if (p2) mem::wr32(p2, bytes > first ? b->mem : 0);
    if (n2) mem::wr32(n2, bytes - first);
    ret(c, DS_OK);
}
void b_Play(cpu::Cpu& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> lk(g_mu);
    b->playing = true;
    b->looping = arg(c, 3) & DSBPLAY_LOOPING;
    ret(c, DS_OK);
}
void b_SetCurrentPosition(cpu::Cpu& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> lk(g_mu);
    if (b->block()) b->pos = (double)(arg(c, 1) / b->block());
    ret(c, DS_OK);
}
void b_SetFormat(cpu::Cpu& c) {
    Buffer* b = buf(c);
    uint32_t wfx = arg(c, 1);
    b->channels = mem::rd16(wfx + 2);
    b->rate = mem::rd32(wfx + 4);
    b->bits = mem::rd16(wfx + 14);
    ret(c, DS_OK);
}
void b_SetVolume(cpu::Cpu& c) {
    buf(c)->volume = (int32_t)arg(c, 1);
    ret(c, DS_OK);
}
void b_SetPan(cpu::Cpu& c) {
    buf(c)->pan = (int32_t)arg(c, 1);
    ret(c, DS_OK);
}
void b_SetFrequency(cpu::Cpu& c) {
    buf(c)->freq = arg(c, 1);
    ret(c, DS_OK);
}
void b_Stop(cpu::Cpu& c) {
    Buffer* b = buf(c);
    std::lock_guard<std::mutex> lk(g_mu);
    b->playing = false;
    ret(c, DS_OK);
}

const com::Method kBufferMethods[] = {
    {"IDirectSoundBuffer::QueryInterface", 12, nullptr, DSERR_NOINTERFACE},
    {"IDirectSoundBuffer::AddRef", 4, com::AddRef, 0},
    {"IDirectSoundBuffer::Release", 4, b_Release, 0},
    {"IDirectSoundBuffer::GetCaps", 8, b_GetCaps, 0},
    {"IDirectSoundBuffer::GetCurrentPosition", 12, b_GetCurrentPosition, 0},
    {"IDirectSoundBuffer::GetFormat", 16, b_GetFormat, 0},
    {"IDirectSoundBuffer::GetVolume", 8, b_GetVolume, 0},
    {"IDirectSoundBuffer::GetPan", 8, b_GetPan, 0},
    {"IDirectSoundBuffer::GetFrequency", 8, b_GetFrequency, 0},
    {"IDirectSoundBuffer::GetStatus", 8, b_GetStatus, 0},
    {"IDirectSoundBuffer::Initialize", 12, ds_ok, 0},
    {"IDirectSoundBuffer::Lock", 32, b_Lock, 0},
    {"IDirectSoundBuffer::Play", 16, b_Play, 0},
    {"IDirectSoundBuffer::SetCurrentPosition", 8, b_SetCurrentPosition, 0},
    {"IDirectSoundBuffer::SetFormat", 8, b_SetFormat, 0},
    {"IDirectSoundBuffer::SetVolume", 8, b_SetVolume, 0},
    {"IDirectSoundBuffer::SetPan", 8, b_SetPan, 0},
    {"IDirectSoundBuffer::SetFrequency", 8, b_SetFrequency, 0},
    {"IDirectSoundBuffer::Stop", 4, b_Stop, 0},
    {"IDirectSoundBuffer::Unlock", 20, ds_ok, 0},
    {"IDirectSoundBuffer::Restore", 4, ds_ok, 0},
};

std::mutex g_vt_mu;
uint32_t dsound_vtable() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kDSoundMethods, sizeof kDSoundMethods / sizeof kDSoundMethods[0]);
    return vt;
}
uint32_t buffer_vtable() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kBufferMethods, sizeof kBufferMethods / sizeof kBufferMethods[0]);
    return vt;
}
}  // namespace

// DirectSoundCreate (imported by ordinal 1) and DirectSoundCreate8 (ordinal 11).
HLE_EXPORT(dsound, DirectSoundCreate, 12) {
    DSound* ds = new DSound();
    mem::wr32(arg(c, 1), com::wrap(ds, dsound_vtable()));
    LOGI("DirectSoundCreate");
    ret(c, DS_OK);
}
HLE_EXPORT(dsound, DirectSoundCreate8, 12) {
    DSound* ds = new DSound();
    mem::wr32(arg(c, 1), com::wrap(ds, dsound_vtable()));
    ret(c, DS_OK);
}
namespace {
const hle::OrdinalAlias kDsOrdinals[] = {{"dsound", 1, "DirectSoundCreate"}, {"dsound", 11, "DirectSoundCreate8"}};
}
