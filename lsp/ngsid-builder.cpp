/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ngsid-builder.h"
#include "ngsid-emu.h"

#include <new>

unsigned int NgSidBuilder::create(unsigned int sids)
{
    m_status = true;
    unsigned int count;
    for (count = 0; count < sids; count++)
    {
        try
        {
            libsidplayfp::NgSid *e = new libsidplayfp::NgSid(this);
            e->setConfig(m_cfg);
            sidobjs.insert(e);
        }
        catch (std::bad_alloc const &)
        {
            m_errorBuffer.assign(name()).append(" ERROR: Unable to create ngSid object");
            m_status = false;
            break;
        }
    }
    return count;
}

const char *NgSidBuilder::credits() const { return libsidplayfp::NgSid::getCredits(); }

void NgSidBuilder::setConfig(const ngsid_config &cfg)
{
    m_cfg = cfg;
    for (libsidplayfp::sidemu *e : sidobjs)
        static_cast<libsidplayfp::NgSid *>(e)->setConfig(m_cfg);
}

ngsid *NgSidBuilder::synth() const
{
    for (libsidplayfp::sidemu *e : sidobjs)
        return static_cast<libsidplayfp::NgSid *>(e)->synth();
    return nullptr;
}

void NgSidBuilder::setNoVolBurst(bool on)
{
    for (libsidplayfp::sidemu *e : sidobjs)
        static_cast<libsidplayfp::NgSid *>(e)->setNoVolBurst(on);
}

libsidplayfp::NgSid *NgSidBuilder::chip() const
{
    for (libsidplayfp::sidemu *e : sidobjs)
        return static_cast<libsidplayfp::NgSid *>(e);
    return nullptr;
}

double NgSidBuilder::clockHz() const
{
    libsidplayfp::NgSid *c = chip();
    return c ? c->clockHz() : 985248.0;
}
