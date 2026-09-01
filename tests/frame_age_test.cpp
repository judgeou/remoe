#include "frame_age.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr double kRtpRolloverUs = 4'294'967'296.0 * 1'000'000.0 / 90'000.0;

void expect_near(std::string_view name, double actual, double expected,
                 double tolerance = 0.01) {
    if (std::abs(actual - expected) <= tolerance) return;
    std::cerr << name << ": expected " << expected << " ms, got " << actual << " ms\n";
    throw std::runtime_error("frame age assertion failed");
}

} // namespace

int main() {
    constexpr std::int64_t timestamp_us = 1'000'000;
    constexpr std::int64_t actual_age_us = 23'500;

    expect_near("no rollover",
        remoe::frame_age_ms_from_rtp_timestamp(
            timestamp_us + actual_age_us, timestamp_us),
        23.5);

    // A Client joining after one rollover receives the low 32-bit RTP epoch,
    // which previously made a normal frame appear roughly 13.26 hours old.
    expect_near("late join after one rollover",
        remoe::frame_age_ms_from_rtp_timestamp(
            static_cast<std::int64_t>(kRtpRolloverUs) + timestamp_us + actual_age_us,
            timestamp_us),
        23.5);

    expect_near("late join after two rollovers",
        remoe::frame_age_ms_from_rtp_timestamp(
            static_cast<std::int64_t>(2.0 * kRtpRolloverUs) +
                timestamp_us + actual_age_us,
            timestamp_us),
        23.5);

    // The transport still unwraps rollovers seen during this connection. The
    // age calculation must only compensate for the rollover missed at join.
    expect_near("rollover observed after late join",
        remoe::frame_age_ms_from_rtp_timestamp(
            static_cast<std::int64_t>(2.0 * kRtpRolloverUs) +
                timestamp_us + actual_age_us,
            static_cast<std::uint64_t>(kRtpRolloverUs) + timestamp_us),
        23.5);

    expect_near("small negative estimate is clamped",
        remoe::frame_age_ms_from_rtp_timestamp(timestamp_us - 500, timestamp_us),
        0.0);
}
