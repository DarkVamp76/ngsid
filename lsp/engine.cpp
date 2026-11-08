/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.h"
#include "ngsid-builder.h"
#include "ngsid-emu.h"

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidConfig.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>

struct engine {
    sidplayfp player;
    NgSidBuilder *builder = nullptr;
    SidTune *tune = nullptr;
    std::deque<uint8_t> carry;     /* rendered samples not yet handed out (L,R pairs) */
    int rate = 44100;
    std::string error;
    bool failed = false;
};

engine *engine_create(const uint8_t *buf, size_t len, int song, int rate, const ngsid_config *cfg,
                      const engine_opts *opts, char *err, size_t errlen)
{
    auto fail = [&](const char *msg) -> engine * {
        if (err && errlen) snprintf(err, errlen, "%s", msg);
        return nullptr;
    };

    engine *e = new engine;
    e->builder = new NgSidBuilder("ngSid");
    if (cfg) e->builder->setConfig(*cfg);
    if (opts) e->builder->setNoVolBurst(opts->no_vol_burst != 0);

    if (e->builder->create(e->player.info().maxsids()) == 0)
    {
        std::string m = e->builder->error();
        engine_destroy(e);
        return fail(m.c_str());
    }
    if (opts) e->builder->setNoVolBurst(opts->no_vol_burst != 0);

    e->rate = rate;
    e->tune = new SidTune(buf, (uint_least32_t)len);
    if (!e->tune->getStatus())
    {
        std::string m = e->tune->statusString();
        engine_destroy(e);
        return fail(m.c_str());
    }
    e->tune->selectSong(song > 0 ? (unsigned)song : 0);

    SidConfig c = e->player.config();
    c.frequency = (uint_least32_t)rate;
    c.playback = SidConfig::STEREO;
    c.samplingMethod = SidConfig::INTERPOLATE;
    c.fastSampling = false;
    c.sidEmulation = e->builder;
    if (opts && opts->pal_override >= 0)
    {
        c.forceC64Model = true;
        c.defaultC64Model = opts->pal_override ? SidConfig::PAL : SidConfig::NTSC;
    }
    else
    {
        c.defaultC64Model = SidConfig::PAL;
    }
    if (!e->player.config(c))
    {
        std::string m = e->player.error();
        engine_destroy(e);
        return fail(m.c_str());
    }
    if (!e->player.load(e->tune))
    {
        std::string m = e->player.error();
        engine_destroy(e);
        return fail(m.c_str());
    }
    if (opts) e->builder->setNoVolBurst(opts->no_vol_burst != 0);
    return e;
}

void engine_destroy(engine *e)
{
    if (!e) return;
    delete e->tune;
    delete e->builder;      /* after the player has let go of the chips */
    delete e;
}

size_t engine_render(engine *e, uint8_t *raw, size_t frames)
{
    if (e->failed) { memset(raw, 0, frames * 2); return frames; }
    libsidplayfp::NgSid *chip = e->builder->chip();
    const double cps = e->builder->clockHz() / (double)e->rate;
    size_t done = 0;
    int idle = 0;
    while (done < frames && chip)
    {
        /* samples left over from the previous call come first */
        while (done < frames && !e->carry.empty())
        {
            raw[done * 2] = e->carry.front(); e->carry.pop_front();
            raw[done * 2 + 1] = e->carry.front(); e->carry.pop_front();
            done++;
        }
        if (done >= frames) break;
        unsigned cycles = (unsigned)((double)(frames - done) * cps) + 1u;
        if (cycles > 20000u) cycles = 20000u;
        const int n = e->player.play(cycles);
        if (n < 0)
        {
            e->failed = true;
            e->error = e->player.error();
            if (e->error.empty()) e->error = "playback stopped";
            break;
        }
        const short *L = chip->buffer(), *R = chip->bufferR();
        for (int i = 0; i < n; i++)
        {
            e->carry.push_back((uint8_t)(L[i] < 0 ? 0 : (L[i] > 255 ? 255 : L[i])));
            e->carry.push_back((uint8_t)(R[i] < 0 ? 0 : (R[i] > 255 ? 255 : R[i])));
        }
        if (n == 0 && ++idle > 1000) break;
    }
    if (done < frames) memset(raw + done * 2, 0, (frames - done) * 2);
    return frames;
}

ngsid *engine_synth(engine *e) { return e->builder->synth(); }

const char *engine_error(const engine *e) { return e->failed ? e->error.c_str() : nullptr; }

const char *engine_version(void) { return "libsidplayfp"; }
