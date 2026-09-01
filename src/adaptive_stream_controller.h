#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace remoe {

// A deliberately small AIMD controller for Remoe's fixed-rate hardware
// encoders. The client request remains the upper bound; the controller starts
// conservatively and raises the working bitrate only after several clean RTCP
// reports.
class AdaptiveStreamController {
public:
    struct Decision {
        std::uint32_t media_bitrate_bps = 0;
        std::uint64_t pacing_bitrate_bps = 0;
        std::chrono::milliseconds pacing_interval{2};
        bool force_key_frame = false;
        std::string reason;
    };

    struct NetworkFeedback {
        bool receiver_report = false;
        double loss_fraction = 0.0;
        std::uint32_t nack_packets = 0;
        bool pli = false;
        std::optional<std::chrono::milliseconds> round_trip_time;
    };

    struct LocalFeedback {
        double queue_delay_ms = 0.0;
        double scheduler_lateness_ms = 0.0;
        std::uint64_t dropped_batches = 0;
    };

    explicit AdaptiveStreamController(std::uint32_t maximum_bitrate_bps);

    [[nodiscard]] Decision initial_decision() const;
    void observe_network(const NetworkFeedback& feedback);
    void observe_local(const LocalFeedback& feedback);
    [[nodiscard]] std::optional<Decision> take_decision();

private:
    void evaluate_locked(const NetworkFeedback& feedback);
    void publish_locked(std::uint32_t bitrate_bps, bool force_key_frame,
                        std::string reason);
    [[nodiscard]] std::chrono::milliseconds pacing_interval_locked() const;

    mutable std::mutex mutex_;
    const std::uint32_t maximum_bitrate_bps_;
    const std::uint32_t minimum_bitrate_bps_;
    std::uint32_t current_bitrate_bps_;
    std::optional<Decision> pending_decision_;
    std::optional<std::chrono::milliseconds> previous_rtt_;
    std::uint64_t previous_dropped_batches_ = 0;
    double queue_delay_ms_ = 0.0;
    double scheduler_lateness_ms_ = 0.0;
    unsigned clean_reports_ = 0;
    std::chrono::steady_clock::time_point last_decrease_{};
};

} // namespace remoe
