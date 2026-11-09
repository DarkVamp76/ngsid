/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NGSID_BUILDER_H
#define NGSID_BUILDER_H

#include "sidplayfp/sidbuilder.h"
#include <vector>

namespace libsidplayfp { class NgSid; }

extern "C" {
#include "ngsid.h"
}

class NgSidBuilder final : public sidbuilder
{
public:
    NgSidBuilder(const char *name) : sidbuilder(name) { ngsid_config_default(&m_cfg); }
    ~NgSidBuilder() override { remove(); }

    unsigned int availDevices() const override { return 0; }
    unsigned int create(unsigned int sids) override;
    const char *credits() const override;
    void filter(bool) override {}

    void setConfig(const ngsid_config &cfg);
    void setNoVolBurst(bool on);
    ngsid_config &config() { return m_cfg; }
    /* The library hands out chips in the order of its internal pointer set, not in creation order,
     * so "SID 1, 2, 3" is the order in which the player locked them. */
    ngsid *synth() const;           /* SID 1, or null */
    libsidplayfp::NgSid *chip() const;    /* SID 1, or null */
    unsigned count() const { return (unsigned)chips().size(); }
    libsidplayfp::NgSid *chipAt(unsigned n) const { return chips()[n]; }
    void noteLock(libsidplayfp::NgSid *c);
    void noteUnlock(libsidplayfp::NgSid *c);
    double clockHz() const;

private:
    const std::vector<libsidplayfp::NgSid *> &chips() const { return m_locked.empty() ? m_chips : m_locked; }

    ngsid_config m_cfg;
    std::vector<libsidplayfp::NgSid *> m_chips;     /* creation order */
    std::vector<libsidplayfp::NgSid *> m_locked;    /* lock order = SID 1, 2, 3 */
};

#endif
