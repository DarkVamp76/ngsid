/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ngsid-emu.h - the VacSid style synth (ngsid.c) as a libsidplayfp SID emulation.
 *
 * libsidplayfp supplies the cycle exact C64 (6510, VIC-II, CIA, PLA); this class only has to
 * turn timestamped SID register writes into sound. The synth renders in blocks of
 * NGSID_BLOCK samples, so the output lags the machine by one block: writes are queued with
 * their sample position and applied exactly there while a block is rendered. */
#ifndef NGSID_EMU_H
#define NGSID_EMU_H

#include <deque>
#include <vector>
#include <stdint.h>

#include "sidemu.h"
#include "Event.h"
#include "sidplayfp/SidConfig.h"

extern "C" {
#include "ngsid.h"
}

namespace libsidplayfp
{

class NgSid final : public sidemu
{
public:
    static const char *getCredits();

    NgSid(sidbuilder *builder);
    ~NgSid() override;

    bool getStatus() const { return m_status; }

    uint8_t read(uint_least8_t addr) override;
    void write(uint_least8_t addr, uint8_t data) override;
    void reset(uint8_t volume) override;
    void clock() override;
    void sampling(float systemclock, float freq, SidConfig::sampling_method_t method, bool fast) override;
    void model(SidConfig::sid_model_t model, bool digiboost) override;

    /* right channel of the stereo output; buffer() holds the left channel */
    short *bufferR() const { return m_bufferR; }

    /* ngSid specific */
    void setConfig(const ngsid_config &cfg);
    void setNoVolBurst(bool on) { m_noBurst = on; }
    ngsid *synth() const { return m_s; }
    double clockHz() const { return m_clk; }

private:
    struct Op { uint64_t sample; uint8_t kind; uint8_t reg; uint32_t val; };
    enum { OP_WRITE, OP_DIGI, OP_VMOD, OP_HOLD };

    void rebuild();
    void renderBlock();
    uint64_t sampleOf(event_clock_t t) const;
    event_clock_t now() const;
    void queue(uint64_t sample, uint8_t kind, uint8_t reg, uint32_t val);
    void checkBurstEnd(event_clock_t t);
    static void sampleHook(void *user);
    void voiceWrite(unsigned addr, uint8_t data, event_clock_t t, uint64_t at);
    void advance(int v, event_clock_t t);
    uint8_t waveNow(int v) const;

    ngsid *m_s = nullptr;
    ngsid_config m_cfg;
    double m_clk = 985248.0, m_rate = 44100.0;
    bool m_haveClock = false;

    event_clock_t m_t0 = 0;
    bool m_t0Set = false;
    uint64_t m_served = 0, m_rendered = 0, m_cur = 0;
    std::deque<Op> m_ops;
    std::deque<uint8_t> m_fifo;         /* interleaved L,R raw synth bytes (lag included) */
    int m_buffersize = 0;
    short *m_bufferR = nullptr;

    /* cycle exact phase counters: waveform-switching digis need the oscillator value at the
     * moment the waveform is switched off, which a per-sample synth cannot provide */
    struct VState {
        uint32_t acc = 0;               /* 24 bit phase accumulator */
        event_clock_t last = 0;         /* time acc is valid for */
        uint8_t reg[7] = {0, 0, 0, 0, 0, 0, 0};
        event_clock_t lastCtrl = 0;
        int fast = 0;                   /* consecutive fast control register writes */
        bool hold = false;
    } m_vs[3];

    /* $D418 digi handling (VacSid): volume-register samples */
    bool m_noBurst = false;
    int m_volDigi = 0, m_volFast = 0;
    event_clock_t m_volLast = 0;
    uint8_t m_volBase = 15, m_volPrev = 15, m_volNib = 15, m_mainVol = 15;
};

}

#endif
