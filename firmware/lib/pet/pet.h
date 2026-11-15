/*
 * pet — the creature that lives on the watch. Pure logic: no Arduino, no LVGL,
 * no RTC, so it runs in milliseconds on the host and can be tested for real.
 *
 * Rules of the world (single source of truth, stated as constants so the tests
 * assert the *rules* rather than re-deriving whatever the code happens to do):
 *
 *   movement feeds it      10 steps  -> +1 satiety
 *   meters cap             satiety, energy <= 100
 *   neglect costs          -3 satiety/hour, -5 energy/hour
 *   a day at the goal      >= 1000 steps in a day keeps the streak alive
 *   it grows               2500 / 25000 / 100000 lifetime steps = new stage
 *   how it feels           mood = average of satiety and energy, +10 for a day
 *                          that met the step goal, capped at 100
 *   how far it has come    lifetimeKm(stride): lifetime steps x stride, and the
 *                          stride is passed in because it is a body measurement
 *                          that belongs to the caller, not to this module
 *
 * Consumption in this world happens on touch (see the firmware); this module
 * owns only state transitions.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace pet {

enum class Stage : uint8_t {
    Bloblet = 0,   // just hatched: a single soft blob
    Sprout,        // gains a nub on top
    Bloom,         // wider, with side bumps
    Ancient        // heavy, sagging, edged
};

constexpr uint32_t kMoodMax    = 100;
constexpr uint32_t kMoodGoalBonus = 10;
constexpr uint32_t kSatietyMax = 100;
constexpr uint32_t kEnergyMax  = 100;
constexpr uint32_t kStepsPerSatietyPoint = 10;
constexpr uint32_t kSatietyDecayPerHour = 3;
constexpr uint32_t kEnergyDecayPerHour  = 5;
constexpr uint32_t kDailyGoalSteps = 1000;
constexpr uint32_t kSecondsPerDay = 86400;

constexpr uint32_t kStage2Steps = 2500;
constexpr uint32_t kStage3Steps = 25000;
constexpr uint32_t kStage4Steps = 100000;

struct State {
    uint32_t stepsToday    = 0;
    uint32_t stepsLifetime = 0;
    uint32_t satiety       = 50;
    uint32_t energy        = 50;
    uint32_t secIntoDay    = 0;
    uint32_t ageSeconds    = 0;
    uint32_t streakDays    = 0;
    Stage    stage         = Stage::Bloblet;
};

class Pet {
public:
    Pet() = default;
    explicit Pet(const State &s) : s_(s) {}

    // A step delta arrives from the accelerometer's pedometer.
    void onSteps(uint32_t steps);

    // Time passes (seconds since the last call).
    void advance(uint32_t seconds);

    // Movement converted to distance, no GPS involved.
    float lifetimeKm(float strideMetres) const;

    // 0..100, derived from how well cared for it is.
    uint32_t mood() const;

    const State &state() const { return s_; }

    Stage stage() const { return s_.stage; }

    // True exactly once per stage change, so the UI can celebrate it one time
    // and then clear the flag.
    bool justEvolved() const { return evolved_; }
    void clearEvolution() { evolved_ = false; }

    // Text form, so NVS and the phone's web page can both read it:
    // "<stepsToday> <stepsLifetime> <satiety> <energy> <secIntoDay> <ageSeconds> <streak> <stage>"
    // Returns the number of characters written (excluding the terminator), or 0
    // if it did not fit.
    size_t serialize(char *out, size_t cap) const;

    // False (and leaves out untouched) on malformed input — never throws, and
    // never leaves a half-parsed creature behind.
    static bool deserialize(const char *in, Pet &out);

private:
    State s_{};
    bool  evolved_ = false;
};

}  // namespace pet
