// SPDX-License-Identifier: GPL-2.0-or-later
#include "renderprofile.h"
#include <QJsonArray>
#include <stdexcept>

int main()
{
    KWin::RenderProfile profile;
    profile.begin();
    profile.end({320, 180}, true, true);
    const auto snapshot = profile.results(true);
    if (snapshot["samples"].toArray().size() != 1 || !profile.results()["samples"].toArray().isEmpty())
        throw std::runtime_error("Profile reset must return the previous samples and then clear them");
}
