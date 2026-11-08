/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NGSID_BUILDER_H
#define NGSID_BUILDER_H

#include "sidplayfp/sidbuilder.h"

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
    ngsid *synth() const;           /* first chip, or null */
    libsidplayfp::NgSid *chip() const;    /* first chip, or null */
    double clockHz() const;

private:
    ngsid_config m_cfg;
};

#endif
