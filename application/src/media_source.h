#pragma once
#include "index.h"
#include <functional>

// Call off the UI thread: a compatible copy is fully hashed before use and its
// metadata is checked again afterwards. Unmapped originals keep their exact path.
// Progress reports cumulative bytes for each full hash pass, beginning at zero.
// It is activity only: successful completion still requires the returned path.
QString verifiedMediaPath(const Track &track, QString *error = nullptr,
                          const std::function<void(qint64)> &progress = {});
