/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ngsid-builder.h"
#include "ngsid-emu.h"

#include <new>

unsigned int NgSidBuilder::create(unsigned int sids)
{
    m_status = true;
    m_chips.clear();
    m_locked.clear();
    unsigned int count;
    for (count = 0; count < sids; count++)
    {
        try
        {
            libsidplayfp::NgSid *e = new libsidplayfp::NgSid(this);
            e->setConfig(m_cfg);
            sidobjs.insert(e);
            m_chips.push_back(e);
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
    return chips().empty() ? nullptr : chips()[0]->synth();
}

void NgSidBuilder::noteLock(libsidplayfp::NgSid *c) { m_locked.push_back(c); }

void NgSidBuilder::noteUnlock(libsidplayfp::NgSid *c)
{
    for (size_t i = 0; i < m_locked.size(); i++)
        if (m_locked[i] == c) { m_locked.erase(m_locked.begin() + (long)i); break; }
}

void NgSidBuilder::setNoVolBurst(bool on)
{
    for (libsidplayfp::sidemu *e : sidobjs)
        static_cast<libsidplayfp::NgSid *>(e)->setNoVolBurst(on);
}

libsidplayfp::NgSid *NgSidBuilder::chip() const
{
    return chips().empty() ? nullptr : chips()[0];
}

double NgSidBuilder::clockHz() const
{
    libsidplayfp::NgSid *c = chip();
    return c ? c->clockHz() : 985248.0;
}
