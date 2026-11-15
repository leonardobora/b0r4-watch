#include "pet.h"

#include <cstdio>

namespace pet {

// Slice 1: movement feeds it. Only this seam is implemented — decay, evolution
// and persistence get their own failing test first.
void Pet::onSteps(uint32_t steps)
{
    s_.stepsToday    += steps;
    s_.stepsLifetime += steps;

    s_.satiety += steps / kStepsPerSatietyPoint;
    if (s_.satiety > kSatietyMax) s_.satiety = kSatietyMax;

    // Slice 3: growing up is a function of lifetime movement and only ever
    // upward, so a crossing announces itself exactly once.
    Stage reached = s_.stage;
    if (s_.stepsLifetime >= kStage4Steps)      reached = Stage::Ancient;
    else if (s_.stepsLifetime >= kStage3Steps) reached = Stage::Bloom;
    else if (s_.stepsLifetime >= kStage2Steps) reached = Stage::Sprout;

    if (reached != s_.stage) {
        s_.stage = reached;
        evolved_ = true;
    }
}

// Slice 2: time passes. Decay is proportional to elapsed time (floored), meters
// bottom out at zero, and each midnight either keeps or breaks the streak.
void Pet::advance(uint32_t seconds)
{
    s_.ageSeconds += seconds;

    uint32_t satietyDrop = (uint32_t)(((uint64_t)seconds * kSatietyDecayPerHour) / 3600ULL);
    uint32_t energyDrop  = (uint32_t)(((uint64_t)seconds * kEnergyDecayPerHour) / 3600ULL);
    s_.satiety = (satietyDrop >= s_.satiety) ? 0 : s_.satiety - satietyDrop;
    s_.energy  = (energyDrop  >= s_.energy)  ? 0 : s_.energy  - energyDrop;

    s_.secIntoDay += seconds;
    while (s_.secIntoDay >= kSecondsPerDay) {
        s_.secIntoDay -= kSecondsPerDay;
        if (s_.stepsToday >= kDailyGoalSteps) s_.streakDays++;
        else                                  s_.streakDays = 0;
        s_.stepsToday = 0;
    }
}

// Distance travelled since hatching. The stride is the caller's business (it is
// a body measurement, and the UI owns the default), so it arrives as an
// argument; a nonsense stride means "no distance" rather than a negative one.
float Pet::lifetimeKm(float strideMetres) const
{
    if (strideMetres <= 0.0f) return 0.0f;
    return (float)s_.stepsLifetime * strideMetres / 1000.0f;
}

// Mood is the average of the two meters — being fed and being rested — plus a
// nudge of 10 for a day that has already met its step goal. It is a percentage,
// so it caps at 100, and a neglected creature sits at 0 rather than wrapping.
uint32_t Pet::mood() const
{
    uint32_t m = (s_.satiety + s_.energy) / 2;
    if (s_.stepsToday >= kDailyGoalSteps) m += 10;
    return m > kMoodMax ? kMoodMax : m;
}

// Slice 4: a single line of text, so NVS and the watch's own web page can both
// carry it. Refuses to truncate: a partial record is worse than none.
size_t Pet::serialize(char *out, size_t cap) const
{
    if (!out || cap == 0) return 0;

    int n = snprintf(out, cap, "%lu %lu %lu %lu %lu %lu %lu %u",
                     (unsigned long)s_.stepsToday,
                     (unsigned long)s_.stepsLifetime,
                     (unsigned long)s_.satiety,
                     (unsigned long)s_.energy,
                     (unsigned long)s_.secIntoDay,
                     (unsigned long)s_.ageSeconds,
                     (unsigned long)s_.streakDays,
                     (unsigned)  s_.stage);

    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool Pet::deserialize(const char *in, Pet &out)
{
    if (!in) return false;

    unsigned long a, b, c, d, e, f, g;
    unsigned stage = 0;
    if (sscanf(in, "%lu %lu %lu %lu %lu %lu %lu %u", &a, &b, &c, &d, &e, &f, &g, &stage) != 8) {
        return false;
    }
    if (stage > (unsigned)Stage::Ancient) return false;

    // Build to one side and assign only on success, so a failed parse leaves the
    // caller's creature exactly as it was.
    State s;
    s.stepsToday    = (uint32_t)a;
    s.stepsLifetime = (uint32_t)b;
    s.satiety       = (uint32_t)c;
    s.energy        = (uint32_t)d;
    s.secIntoDay    = (uint32_t)e;
    s.ageSeconds    = (uint32_t)f;
    s.streakDays    = (uint32_t)g;
    s.stage         = (Stage)stage;

    out = Pet(s);
    return true;
}

}  // namespace pet
