#include "workout.h"

#include <cmath>
#include <cstdio>

namespace workout {

float Session::metFor(Activity a)
{
    switch (a) {
        case Activity::Walking: return kMetWalking;
        case Activity::Running: return kMetRunning;
        case Activity::Still:   return kMetStill;
        case Activity::Unknown: return kMetStill;
    }
    return kMetStill;
}

float Session::strideFor(Activity a) const
{
    switch (a) {
        case Activity::Walking: return walkingStrideM();
        case Activity::Running: return runningStrideM();
        // Steps that land while the sensor says "still" are attributed to the
        // walking stride: conservative, and it cannot invent a long run.
        case Activity::Still:   return walkingStrideM();
        case Activity::Unknown: return walkingStrideM();
    }
    return walkingStrideM();
}

void Session::start()
{
    t_ = Totals{};
    last_ = Activity::Still;
    milestoneSec_ = 0;
    pulses_ = 0;
    running_ = true;
}

void Session::stop()
{
    running_ = false;
}

void Session::sample(Activity a, uint32_t stepDelta, uint32_t dtSec)
{
    if (!running_) return;

    last_ = a;
    if (dtSec > kMaxTickSec) dtSec = kMaxTickSec;

    // Steps are counted even in a still tick: the sensor can report a step that
    // landed between classifications, and dropping it would lose real distance.
    // Distance accumulates per tick, because the stride depends on how this tick
    // was classified: the same step is worth 0.735 m walking and 1.15 m running.
    t_.steps += stepDelta;
    t_.km += (float)stepDelta * strideFor(a) / 1000.0f;

    const bool working = (a == Activity::Walking || a == Activity::Running);
    if (working) {
        t_.activeSec += dtSec;
        t_.kcal += metFor(a) * weightKg_ * ((float)dtSec / 3600.0f);
    } else {
        t_.restSec += dtSec;
    }

    while (t_.activeSec >= milestoneSec_ + kMilestoneActiveSec &&
           pulses_ < kMaxPendingPulses) {
        milestoneSec_ += kMilestoneActiveSec;
        pulses_++;
    }
}

bool Session::takeMilestonePulse()
{
    if (pulses_ == 0) return false;
    pulses_--;
    return true;
}

bool Session::serialize(char *out, size_t n) const
{
    if (!out || n == 0) return false;
    int written = snprintf(out, n, "W1 %lu %lu %lu %lu %lu",
                           (unsigned long)t_.activeSec,
                           (unsigned long)t_.restSec,
                           (unsigned long)t_.steps,
                           (unsigned long)lroundf(t_.km * 1000.0f),
                           (unsigned long)lroundf(t_.kcal * 10.0f));
    if (written <= 0 || (size_t)written >= n) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool Session::deserialize(const char *in, Totals &out)
{
    if (!in) return false;
    unsigned long active = 0, rest = 0, steps = 0, meters = 0, kcal10 = 0;
    if (sscanf(in, "W1 %lu %lu %lu %lu %lu", &active, &rest, &steps, &meters, &kcal10) != 5) {
        return false;
    }
    out.activeSec = (uint32_t)active;
    out.restSec   = (uint32_t)rest;
    out.steps     = (uint32_t)steps;
    out.km        = (float)meters / 1000.0f;
    out.kcal      = (float)kcal10 / 10.0f;
    return true;
}

}  // namespace workout
