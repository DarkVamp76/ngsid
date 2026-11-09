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

struct engine {
    sidplayfp player;
    NgSidBuilder *builder = nullptr;
    SidTune *tune = nullptr;
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

    if (e->builder->create(e->player.info().maxsids()) == 0)
    {
        std::string m = e->builder->error();
        engine_destroy(e);
        return fail(m.c_str());
    }
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
    c.defaultSidModel = SidConfig::MOS6581;     /* a tune header can ask for the 8580 */
    if (opts && opts->sid_model >= 0)
    {
        c.forceSidModel = true;
        c.defaultSidModel = opts->sid_model ? SidConfig::MOS8580 : SidConfig::MOS6581;
    }
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
    return e;
}

void engine_destroy(engine *e)
{
    if (!e) return;
    delete e->tune;
    delete e->builder;      /* after the player has let go of the chips */
    delete e;
}

unsigned engine_sid_count(const engine *e)
{
    unsigned n = e->tune ? e->tune->getInfo()->sidChips() : 1;
    if (n > e->builder->count()) n = e->builder->count();
    return n ? n : 1;
}

int engine_sid_model(const engine *e, unsigned n)
{
    return n < e->builder->count() ? e->builder->chipAt(n)->sidModel() : 0;
}

/* the mixing of several chips into stereo is done by the library; this call is deprecated
 * upstream in favour of per-chip buffers, but it is the one that mixes for us */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
size_t engine_render(engine *e, int16_t *out, size_t frames)
{
    if (e->failed) { memset(out, 0, frames * 4); return frames; }
    size_t done = 0;
    int idle = 0;
    while (done < frames)
    {
        const unsigned want = (unsigned)((frames - done) * 2);
        const unsigned n = e->player.play((short *)(out + done * 2), want);
        if (n == 0)
        {
            if (++idle > 100)
            {
                e->failed = true;
                e->error = e->player.error();
                if (e->error.empty()) e->error = "playback stopped";
                break;
            }
            continue;
        }
        done += n / 2;
    }
    if (done < frames) memset(out + done * 2, 0, (frames - done) * 4);
    return frames;
}
#pragma GCC diagnostic pop

void engine_mute(engine *e, unsigned mask)
{
    for (unsigned n = 0; n < e->builder->count(); n++)
    {
        libsidplayfp::NgSid *c = e->builder->chipAt(n);
        for (unsigned v = 0; v < 4; v++) c->voice(v, (mask >> v) & 1u);
    }
}

ngsid *engine_synth(engine *e) { return e->builder->synth(); }

const char *engine_error(const engine *e) { return e->failed ? e->error.c_str() : nullptr; }

const char *engine_version(void) { return "libsidplayfp"; }
