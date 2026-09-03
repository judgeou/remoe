#include "adaptive_stream_controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace remoe {
namespace {

constexpr std::uint32_t kAbsoluteMinimumBitrate = 2'000'000;
// A complete video frame cannot be decoded until its last RTP packet arrives.
// A 2x wire rate halves the packet-drain part of a frame interval while still
// smoothing the encoder's frame-sized bursts on ordinary WAN links.
constexpr double kPacingMultiplier = 2.0;
constexpr auto kDecreaseCooldown = std::chrono::seconds(1);

std::uint32_t scaled_bitrate(std::uint32_t bitrate, double factor) {
    return static_cast<std::uint32_t>(std::llround(static_cast<double>(bitrate) * factor));
}

} // namespace

AdaptiveStreamController::AdaptiveStreamController(std::uint32_t maximum_bitrate_bps)
    : maximum_bitrate_bps_(maximum_bitrate_bps),
      minimum_bitrate_bps_((std::min)(maximum_bitrate_bps, kAbsoluteMinimumBitrate)),
      current_bitrate_bps_(maximum_bitrate_bps) {}

AdaptiveStreamController::Decision AdaptiveStreamController::initial_decision() const {
    std::lock_guard lock(mutex_);
    Decision decision;
    decision.media_bitrate_bps = current_bitrate_bps_;
    decision.pacing_bitrate_bps = static_cast<std::uint64_t>(
        static_cast<double>(current_bitrate_bps_) * kPacingMultiplier);
    decision.pacing_interval = pacing_interval_locked();
    decision.reason = "client requested session rate";
    return decision;
}

void AdaptiveStreamController::observe_network(const NetworkFeedback& feedback) {
    std::lock_guard lock(mutex_);
    evaluate_locked(feedback);
}

void AdaptiveStreamController::observe_local(const LocalFeedback& feedback) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto previous_interval = pacing_interval_locked();
    queue_delay_ms_ = (std::max)(0.0, feedback.queue_delay_ms);
    scheduler_lateness_ms_ = (std::max)(0.0, feedback.scheduler_lateness_ms);
    if (feedback.dropped_batches > previous_dropped_batches_) {
        previous_dropped_batches_ = feedback.dropped_batches;
        clean_reports_ = 0;
        if (decrease_allowed_locked(now)) {
            last_decrease_ = now;
            publish_locked((std::max)(minimum_bitrate_bps_,
                                      scaled_bitrate(current_bitrate_bps_, 0.85)),
                           true, "bounded pacer queue dropped a video frame");
        }
    } else if (queue_delay_ms_ >= 100.0 && decrease_allowed_locked(now)) {
        clean_reports_ = 0;
        last_decrease_ = now;
        publish_locked((std::max)(minimum_bitrate_bps_,
                                  scaled_bitrate(current_bitrate_bps_, 0.90)),
                       false, "sustained host pacing delay");
    } else if (pacing_interval_locked() != previous_interval) {
        publish_locked(current_bitrate_bps_, false,
                       "host scheduler selected a safer pacing interval");
    }
}

std::optional<AdaptiveStreamController::Decision>
AdaptiveStreamController::take_decision() {
    std::lock_guard lock(mutex_);
    auto decision = std::move(pending_decision_);
    pending_decision_.reset();
    return decision;
}

void AdaptiveStreamController::evaluate_locked(const NetworkFeedback& feedback) {
    const auto now = std::chrono::steady_clock::now();
    bool rtt_growth = false;
    if (feedback.round_trip_time && previous_rtt_) {
        const auto previous_ms = previous_rtt_->count();
        const auto current_ms = feedback.round_trip_time->count();
        rtt_growth = current_ms > previous_ms + 20 &&
                     current_ms * 2 > previous_ms * 3;
    }
    if (feedback.round_trip_time) previous_rtt_ = feedback.round_trip_time;

    const bool severe = feedback.loss_fraction >= 0.05;
    const bool moderate = feedback.loss_fraction >= 0.02 ||
                          feedback.nack_packets >= 8 ||
                          rtt_growth;

    if (severe) {
        clean_reports_ = 0;
        if (!decrease_allowed_locked(now)) return;
        last_decrease_ = now;
        const auto reduced = (std::max)(minimum_bitrate_bps_,
                                        scaled_bitrate(current_bitrate_bps_, 0.85));
        publish_locked(reduced, false, "severe receiver loss");
        return;
    }
    if (moderate) {
        clean_reports_ = 0;
        if (!decrease_allowed_locked(now)) return;
        last_decrease_ = now;
        publish_locked((std::max)(minimum_bitrate_bps_,
                                  scaled_bitrate(current_bitrate_bps_, 0.90)),
                       false, "loss, NACK, RTT, or pacing delay increased");
        return;
    }

    if (!feedback.receiver_report) return;

    ++clean_reports_;
    if (clean_reports_ < 3 || current_bitrate_bps_ >= maximum_bitrate_bps_ ||
        (last_decrease_ != std::chrono::steady_clock::time_point{} &&
         now - last_decrease_ < std::chrono::seconds(3))) {
        return;
    }
    clean_reports_ = 0;
    const std::uint32_t additive = (std::max)(500'000u,
        scaled_bitrate(current_bitrate_bps_, 0.05));
    publish_locked((std::min)(maximum_bitrate_bps_, current_bitrate_bps_ + additive),
                   false, "three clean receiver reports");
}

bool AdaptiveStreamController::decrease_allowed_locked(
    std::chrono::steady_clock::time_point now) const {
    return last_decrease_ == std::chrono::steady_clock::time_point{} ||
           now - last_decrease_ >= kDecreaseCooldown;
}

void AdaptiveStreamController::publish_locked(std::uint32_t bitrate_bps,
                                               bool force_key_frame,
                                               std::string reason) {
    bitrate_bps = (std::clamp)(bitrate_bps, minimum_bitrate_bps_, maximum_bitrate_bps_);
    const auto interval = pacing_interval_locked();
    if (bitrate_bps == current_bitrate_bps_ && pending_decision_ &&
        pending_decision_->pacing_interval == interval && !force_key_frame) {
        return;
    }
    current_bitrate_bps_ = bitrate_bps;
    Decision decision;
    decision.media_bitrate_bps = bitrate_bps;
    decision.pacing_bitrate_bps = static_cast<std::uint64_t>(
        static_cast<double>(bitrate_bps) * kPacingMultiplier);
    decision.pacing_interval = interval;
    decision.force_key_frame = force_key_frame;
    decision.reason = std::move(reason);
    pending_decision_ = std::move(decision);
}

std::chrono::milliseconds AdaptiveStreamController::pacing_interval_locked() const {
    // At high rates a longer interval necessarily creates a larger UDP burst.
    // Never trade scheduler overhead for a 5 ms burst that cannot sustain the
    // requested throughput with a small RTP batch.
    if (scheduler_lateness_ms_ >= 4.0 && current_bitrate_bps_ <= 6'000'000u) {
        return std::chrono::milliseconds(5);
    }
    if (scheduler_lateness_ms_ >= 2.0 && current_bitrate_bps_ <= 10'000'000u) {
        return std::chrono::milliseconds(3);
    }
    return std::chrono::milliseconds(2);
}

} // namespace remoe
