#pragma once
#include "index.h"
#include <functional>

struct SourceRepairReply {
    Track mapped;
    QString error;
    bool paused = false;
    bool needsConnection = false;
};
using SourceRepairCompletion = std::function<void(SourceRepairReply)>;
using SourceRepairHandler = std::function<void(const Track &, SourceRepairCompletion)>;
