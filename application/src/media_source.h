#pragma once
#include "index.h"
#include <functional>

// Call off the UI thread: a compatible copy is fully hashed on its first use and
// after an opened-file identity/metadata change. Unmapped originals keep their exact path.
// Progress reports cumulative bytes for each full hash pass, beginning at zero.
// Cache hits report no positive bytes. Successful completion requires the returned path.
// An explicit local, per-profile SQLite path shares receipts across processes and
// restarts. Empty uses a bounded process cache; unusable stores fall back to hashing.
QString verifiedMediaPath(const Track &track, QString *error = nullptr,
                          const std::function<void(qint64)> &progress = {},
                          const QString &verificationCachePath = {});
