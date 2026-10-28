#include "background_policy.h"
#include <cmath>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_DARWIN)
#include <mach/mach.h>
#include <mach/mach_host.h>
#endif

bool PressureGate::observe(double load) {
    if (!std::isfinite(load) || load < 0 || load > 1)
        return busy_;
    if (load >= .80) {
        ++hot_;
        cool_ = 0;
        if (hot_ >= 2)
            busy_ = true;
    } else if (load <= .55) {
        ++cool_;
        hot_ = 0;
        if (cool_ >= 3)
            busy_ = false;
    } else {
        hot_ = cool_ = 0;
    }
    return busy_;
}
void PressureGate::reset() {
    hot_ = cool_ = 0;
    busy_ = false;
}
void SystemLoadSampler::reset() {
    total_ = idle_ = 0;
}
double SystemLoadSampler::sample() {
    quint64 total = 0, idle = 0;
#if defined(Q_OS_WIN)
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
        return -1;
    auto ticks = [](const FILETIME &t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    idle = ticks(idleTime);
    total = ticks(kernelTime) + ticks(userTime);
#elif defined(Q_OS_DARWIN)
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    const auto host = mach_host_self();
    const auto result =
        host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count);
    mach_port_deallocate(mach_task_self(), host);
    if (result != KERN_SUCCESS)
        return -1;
    for (int state = 0; state < CPU_STATE_MAX; ++state)
        total += cpu.cpu_ticks[state];
    idle = cpu.cpu_ticks[CPU_STATE_IDLE];
#else
    return -1;
#endif
    const auto priorTotal = total_, priorIdle = idle_;
    total_ = total;
    idle_ = idle;
    if (!priorTotal || total <= priorTotal || idle < priorIdle)
        return -1;
    return qBound(0.0, 1.0 - double(idle - priorIdle) / double(total - priorTotal), 1.0);
}
