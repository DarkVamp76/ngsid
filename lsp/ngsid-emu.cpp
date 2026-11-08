/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ngsid-emu.h"

#include <cmath>
#include <cstring>

namespace libsidplayfp
{

static const int LAG = NGSID_BLOCK;     /* frames of delay between machine time and audio */

const char *NgSid::getCredits()
{
    return "ngSid engine:\n\tVacSid style synthesis after Oliver Blasnik (R0M/Vacuum)\n";
}

NgSid::NgSid(sidbuilder *builder) : sidemu(builder)
{
    ngsid_config_default(&m_cfg);
    m_cfg.stereo = 1;
    m_cfg.sample_rate = 44100;
    rebuild();
    reset(0);
}

NgSid::~NgSid()
{
    ngsid_destroy(m_s);
    delete[] m_buffer;
    delete[] m_bufferR;
}

void NgSid::rebuild()
{
    if (m_s) ngsid_destroy(m_s);
    m_s = ngsid_create(&m_cfg);
    if (m_s) ngsid_set_sample_callback(m_s, &NgSid::sampleHook, this);
    m_status = m_s != nullptr;
}

void NgSid::setConfig(const ngsid_config &cfg)
{
    m_cfg = cfg;
    m_cfg.stereo = 1;
    if (m_haveClock) m_cfg.sample_rate = (int)m_rate;
    m_cfg.pal = m_clk < 1000000.0 ? 1 : 0;
    rebuild();
    reset(0);
}

void NgSid::sampleHook(void *user)
{
    NgSid *self = static_cast<NgSid *>(user);
    while (!self->m_ops.empty() && self->m_ops.front().sample <= self->m_cur)
    {
        const Op &o = self->m_ops.front();
        switch (o.kind)
        {
        case OP_WRITE: ngsid_write(self->m_s, o.reg, (uint8_t)o.val); break;
        case OP_DIGI:  ngsid_set_digi(self->m_s, o.val); break;
        case OP_HOLD:  ngsid_set_hold(self->m_s, o.reg, (int)(o.val >> 8), (uint8_t)(o.val & 0xFF)); break;
        case OP_VMOD:  ngsid_set_vol_mod(self->m_s, (int)(o.val >> 8), o.val & 15u); break;
        }
        self->m_ops.pop_front();
    }
    self->m_cur++;
}

event_clock_t NgSid::now() const
{
    return eventScheduler->getTime(EVENT_CLOCK_PHI1);
}

uint64_t NgSid::sampleOf(event_clock_t t) const
{
    if (t <= m_t0) return 0;
    return (uint64_t)((double)(t - m_t0) * m_rate / m_clk);
}

void NgSid::queue(uint64_t sample, uint8_t kind, uint8_t reg, uint32_t val)
{
    if (sample < m_rendered) sample = m_rendered;    /* too late for audio already produced */
    m_ops.push_back(Op{sample, kind, reg, val});
}

void NgSid::reset(uint8_t volume)
{
    m_accessClk = 0;
    m_ops.clear();
    m_fifo.assign((size_t)LAG * 2, 0);
    m_served = m_rendered = m_cur = 0;
    m_t0Set = false;
    m_volDigi = 0; m_volFast = 0; m_volLast = 0;
    for (auto &v : m_vs) v = VState();
    m_volBase = m_volPrev = m_volNib = m_mainVol = volume & 15;
    if (m_s)
    {
        ngsid_reset(m_s);
        ngsid_set_digi(m_s, 0);
        ngsid_set_vol_mod(m_s, 0, 0);
        ngsid_write(m_s, 0x18, volume);
    }
}

void NgSid::renderBlock()
{
    uint8_t raw[NGSID_BLOCK * 2];
    ngsid_render(m_s, raw, NGSID_BLOCK);     /* applies the queued writes through sampleHook */
    m_fifo.insert(m_fifo.end(), raw, raw + NGSID_BLOCK * 2);
    m_rendered += NGSID_BLOCK;
}

void NgSid::checkBurstEnd(event_clock_t t)
{
    if (m_volDigi && t - m_volLast > 3000)
    {
        /* the volume burst is over: back to the plain volume nibble */
        const uint64_t at = sampleOf(m_volLast + 3000);
        m_volDigi = 0;
        m_mainVol = m_volNib;
        queue(at, OP_DIGI, 0, 0);
        queue(at, OP_VMOD, 0, 0);
        queue(at, OP_WRITE, 0x18, (uint32_t)((ngsid_shadow_reg(m_s, 0x18) & 0xF0) | m_mainVol));
    }
}

uint8_t NgSid::read(uint_least8_t addr)
{
    clock();
    if (addr == 0x1B || addr == 0x1C) return ngsid_read(m_s, addr);
    return 0xFF;
}

void NgSid::write(uint_least8_t addr, uint8_t data)
{
    if (!m_t0Set) { m_t0 = now(); m_t0Set = true; }
    const event_clock_t t = now();
    clock();
    checkBurstEnd(t);
    const uint64_t at = sampleOf(t);

    if (addr == 0x18)
    {
        /* fast $D418 writes are volume register samples (digi); the volume before the burst stays */
        const uint64_t dt = (uint64_t)(t - m_volLast);
        m_volLast = t;
        if (dt >= 1500) { m_volFast = 0; m_volPrev = m_mainVol; }
        else if (m_volFast < 1000) m_volFast++;
        if (!m_noBurst && !m_volDigi && m_volFast >= 3) { m_volDigi = 1; m_volBase = m_volPrev; }
        if (m_volDigi)
        {
            m_volNib = data & 0x0F;
            queue(at, OP_VMOD, 0, (1u << 8) | m_volNib);
            queue(at, OP_WRITE, 0x18, (uint32_t)((data & 0xF0) | m_volBase));
            queue(at, OP_DIGI, 0, (uint32_t)m_volNib * m_volBase * 2u);
        }
        else
        {
            m_mainVol = data & 0x0F;
            queue(at, OP_WRITE, 0x18, data);
        }
    }
    else if (addr < 0x15)
    {
        voiceWrite(addr, data, t, at);
        queue(at, OP_WRITE, addr, data);
    }
    else if (addr < 0x18)
    {
        queue(at, OP_WRITE, addr, data);
    }
}

void NgSid::advance(int v, event_clock_t t)
{
    VState &s = m_vs[v];
    if (t > s.last)
    {
        if (!(s.reg[4] & 0x08))
        {
            const uint32_t f = ((uint32_t)s.reg[1] << 8) | s.reg[0];
            s.acc = (uint32_t)((s.acc + (uint64_t)(t - s.last) * f) & 0xFFFFFFu);
        }
        s.last = t;
    }
}

/* the synth's 8 bit waveform value for the current phase and control register */
uint8_t NgSid::waveNow(int v) const
{
    const VState &s = m_vs[v];
    const uint8_t c = s.reg[4], p = (uint8_t)(s.acc >> 16);
    const uint8_t pw8 = (uint8_t)((((uint32_t)(s.reg[3] & 0x0F) << 8) | s.reg[2]) >> 4);
    uint8_t w = 0;
    if (c & 0x40) w |= (pw8 >= p) ? 0xFF : 0x00;
    if (c & 0x20) w |= p;
    if (c & 0x10)
    {
        uint8_t t = (uint8_t)(p << 1);
        if (p & 0x80) t = (uint8_t)(0xFE - t);
        w |= t;
    }
    return w;
}

void NgSid::voiceWrite(unsigned addr, uint8_t data, event_clock_t t, uint64_t at)
{
    const int v = (int)(addr / 7), r = (int)(addr % 7);
    VState &s = m_vs[v];
    advance(v, t);
    if (r == 4)
    {
        s.fast = (t - s.lastCtrl < 1500) ? (s.fast < 100 ? s.fast + 1 : s.fast) : 0;
        s.lastCtrl = t;
        const uint8_t oldBits = s.reg[4] & 0xF0, newBits = data & 0xF0;
        if (newBits == 0 && oldBits != 0 && !(oldBits & 0x80) && s.fast >= 3)
        {
            /* waveform switched off in a fast sequence: the DAC keeps the last value */
            s.hold = true;
            queue(at, OP_HOLD, (uint8_t)v, (1u << 8) | waveNow(v));
        }
        else if (newBits != 0 && s.hold)
        {
            s.hold = false;
            queue(at, OP_HOLD, (uint8_t)v, 0);
        }
        s.reg[4] = data;
        if (data & 0x08) s.acc = 0;
        return;
    }
    s.reg[r] = data;
}

void NgSid::clock()
{
    if (!eventScheduler) return;
    if (!m_t0Set) { m_t0 = now(); m_t0Set = true; }
    const event_clock_t t = now();
    checkBurstEnd(t);
    const uint64_t upto = sampleOf(t);

    /* a block may only be rendered once every write that falls into it has been queued */
    while (m_rendered + NGSID_BLOCK <= upto) renderBlock();

    if (!m_buffer) return;
    uint64_t avail = upto > m_served ? upto - m_served : 0;
    uint64_t room = (uint64_t)(m_buffersize - m_bufferpos);
    uint64_t n = avail < room ? avail : room;
    for (uint64_t i = 0; i < n; i++)
    {
        const uint8_t l = m_fifo.front(); m_fifo.pop_front();
        const uint8_t r = m_fifo.front(); m_fifo.pop_front();
        m_buffer[m_bufferpos + i] = l;
        m_bufferR[m_bufferpos + i] = r;
    }
    m_bufferpos += (int)n;
    m_served += n;
}

void NgSid::sampling(float systemclock, float freq, SidConfig::sampling_method_t, bool)
{
    m_clk = systemclock;
    m_rate = freq;
    m_haveClock = true;
    m_cfg.sample_rate = (int)freq;
    m_cfg.pal = systemclock < 1000000.0f ? 1 : 0;
    rebuild();
    reset(0);

    delete[] m_buffer;
    delete[] m_bufferR;
    m_buffersize = (int)std::ceil((freq / 1000.f) * 20.f);
    m_buffer = new short[m_buffersize];
    m_bufferR = new short[m_buffersize];
    m_status = m_s != nullptr;
}

void NgSid::model(SidConfig::sid_model_t, bool)
{
    m_status = m_s != nullptr;      /* one synth model only, as VacSid */
}

}
