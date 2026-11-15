/*
 * workout — a gym session that does not pretend to know your heart rate.
 *
 * Pure logic: no Arduino, no LVGL, no sensor library, so it runs on the host in
 * milliseconds and gets tested for real. It is fed *classifications*, not raw
 * motion: the BMA423 on this watch decides walking vs running vs standing still
 * in hardware, and this module only decides what that means.
 *
 * Rules of the world (stated as constants, so the tests assert the *rules*
 * rather than re-deriving whatever the code happens to do):
 *
 *   what counts as work   walking and running. Standing still is rest, and an
 *                         unknown classification is treated as rest too — a
 *                         session should not be credited for what the sensor
 *                         could not identify.
 *   distance              steps x stride, where the stride comes from your height
 *                         (walking 0.415 x height, running 0.65 x height). Running
 *                         steps are longer than walking ones, so distance is
 *                         accumulated per tick with the stride of the activity
 *                         that tick was classified as — it cannot be derived from
 *                         the step total afterwards. With no height on file, the
 *                         compiled-in defaults (0.72 m / 1.15 m) are used.
 *   the profile            height and age are recorded; height drives the strides
 *                         and BMI, and age drives nothing at all. There is no
 *                         heart rate sensor, so age cannot refine any figure here,
 *                         and pretending otherwise would be a lie in the numbers.
 *   energy                MET x body weight x hours, credited ONLY for moving
 *                         time: walking 3.5 MET, running 9.0 MET. A session
 *                         spent standing still therefore burns 0. That is the
 *                         honest figure for "burned by exercise" and it is
 *                         deliberately not a whole-day energy expenditure.
 *   a tick is trusted     10 s. Beyond that it is clamped, so a stalled loop
 *                         cannot inflate a session.
 *   a nudge               every 5 active minutes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace workout {

// Mirrors the sensor's own ActivityType without depending on the sensor library.
enum class Activity : uint8_t {
    Still = 0,
    Walking = 1,
    Running = 2,
    Unknown = 3
};

constexpr float    kMetStill   = 1.0f;
constexpr float    kMetWalking = 3.5f;
constexpr float    kMetRunning = 9.0f;
constexpr float    kDefaultStrideM = 0.72f;        // walking, when no height is known
constexpr float    kDefaultRunningStrideM = 1.15f; // running, when no height is known
constexpr float    kWalkingStrideRatio = 0.415f;   // stride as a fraction of height
constexpr float    kRunningStrideRatio = 0.65f;
constexpr float    kMinHeightM = 1.0f;             // below this, treat height as unknown
constexpr float    kMaxHeightM = 2.5f;
constexpr float    kDefaultWeightKg = 75.0f;
constexpr float    kMinWeightKg = 20.0f;
constexpr uint32_t kMaxTickSec = 10;
constexpr uint32_t kMilestoneActiveSec = 300;   // 5 minutes
constexpr uint8_t  kMaxPendingPulses = 8;

struct Totals {
    uint32_t activeSec = 0;
    uint32_t restSec   = 0;
    uint32_t steps     = 0;
    float    km        = 0.0f;
    float    kcal      = 0.0f;
};

class Session
{
public:
    Session() = default;

    void start();
    void stop();
    bool running() const { return running_; }

    // One tick of sensor truth: what the hardware classified, how many steps
    // landed in it, and how much time it covers. Ignored when not running.
    void sample(Activity a, uint32_t stepDelta, uint32_t dtSec);

    const Totals &totals() const { return t_; }
    Activity lastActivity() const { return last_; }
    uint32_t elapsedSec() const { return t_.activeSec + t_.restSec; }

    // Put a finished session back on the display (loaded from flash) without
    // letting it look live or keep accumulating.
    void restore(const Totals &t) { t_ = t; running_ = false; last_ = Activity::Still; pulses_ = 0; milestoneSec_ = t.activeSec; }

    // Buzz bookkeeping: true once per 5-minute milestone reached, then cleared.
    bool takeMilestonePulse();

    // Body profile. Height is what makes distance honest; age is carried for the
    // page's sake and changes no calculation (see the module comment).
    void  setHeightM(float m)
    {
        heightM_ = (m >= kMinHeightM && m <= kMaxHeightM) ? m : 0.0f;
    }
    float heightM() const { return heightM_; }
    void  setAgeYears(uint16_t y) { ageYears_ = (y >= 5 && y <= 120) ? y : 0; }
    uint16_t ageYears() const { return ageYears_; }

    void  setStrideM(float m) { strideOverrideM_ = (m > 0.1f) ? m : 0.0f; }
    void  setWeightKg(float kg) { weightKg_ = (kg > kMinWeightKg) ? kg : kMinWeightKg; }
    float weightKg() const { return weightKg_; }

    float walkingStrideM() const
    {
        if (strideOverrideM_ > 0.0f) return strideOverrideM_;
        return heightM_ > 0.0f ? heightM_ * kWalkingStrideRatio : kDefaultStrideM;
    }
    float runningStrideM() const
    {
        return heightM_ > 0.0f ? heightM_ * kRunningStrideRatio : kDefaultRunningStrideM;
    }

    // BMI is the one figure the whole profile produces; 0 when height is unknown
    // so the caller can show "unknown" instead of a divide-by-zero number.
    float bmi() const
    {
        if (heightM_ <= 0.0f) return 0.0f;
        return weightKg_ / (heightM_ * heightM_);
    }

    // The finished session, so it can survive a reboot and be shown after the
    // fact. One line, integers only: "W1 <activeSec> <restSec> <steps> <meters> <kcalX10>"
    bool serialize(char *out, size_t n) const;
    static bool deserialize(const char *in, Totals &out);

private:
    static float metFor(Activity a);
    float strideFor(Activity a) const;

    Totals   t_{};
    Activity last_ = Activity::Still;
    bool     running_ = false;
    uint32_t milestoneSec_ = 0;
    uint8_t  pulses_ = 0;
    float    strideOverrideM_ = 0.0f;   // wins over the height-derived stride
    float    heightM_ = 0.0f;
    float    weightKg_ = kDefaultWeightKg;
    uint16_t ageYears_ = 0;
};

}  // namespace workout
