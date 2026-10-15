#pragma once

#include <ctime>
#include <span>
#include <string>
#include <string_view>

namespace imza {

// Time-of-day bucket for the startup greeting. Night spans 22:00-05:00;
// everything else is Day.
enum class TimeBucket { NIGHT, DAY };

TimeBucket time_bucket(int hour);

// The greeting pool for a bucket; stable order, never empty.
std::span<const std::string_view> greeting_pool(TimeBucket bucket);

// One line from the bucket's pool selected by `seed`. Deterministic: the
// same bucket and seed always yield the same line.
std::string greeting_line(TimeBucket bucket, unsigned seed);

// Bucket and pick from local wall-clock time, seeded by the local calendar
// date so the line is stable for the whole day and rotates the next.
std::string greeting_line_for(std::time_t now);

} // namespace imza
