#pragma once
#include <QtGlobal>

// Hysteresis avoids stopping/restarting background jobs around one noisy sample.
class PressureGate {
  public:
    bool observe(double busyFraction);
    void reset();
    bool busy() const {
        return busy_;
    }

  private:
    int hot_ = 0, cool_ = 0;
    bool busy_ = false;
};
class SystemLoadSampler {
  public:
    double sample(); // -1 means not available or the first sample.
    void reset();

  private:
    quint64 total_ = 0, idle_ = 0;
};
