#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace remoe {

// RTP video timestamps use a 90 kHz 32-bit clock. A receiver can unwrap
// rollovers observed during a connection, but it cannot know how many
// rollovers happened before a late connection. Select the timestamp epoch
// nearest to the synchronized Host clock; a real frame cannot reasonably be
// more than half of the roughly 13.26-hour RTP rollover period away.
inline double frame_age_ms_from_rtp_timestamp(
    std::int64_t estimated_host_now_us,
    std::uint64_t received_timestamp_us) noexcept {
    if (received_timestamp_us > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return 0.0;
    }

    constexpr double video_clock_rate = 90'000.0;
    constexpr double rtp_timestamp_range = 4'294'967'296.0;
    constexpr double rollover_us = rtp_timestamp_range * 1'000'000.0 /
        video_clock_rate;
    const double raw_age_us = static_cast<double>(estimated_host_now_us) -
        static_cast<double>(received_timestamp_us);
    const double missed_rollovers = std::round(raw_age_us / rollover_us);
    const double corrected_age_us = raw_age_us - missed_rollovers * rollover_us;
    return (std::max)(corrected_age_us, 0.0) / 1000.0;
}

} // namespace remoe
