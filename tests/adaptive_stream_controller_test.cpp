#include "adaptive_stream_controller.h"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        remoe::AdaptiveStreamController controller(20'000'000);
        const auto initial = controller.initial_decision();
        if (initial.media_bitrate_bps != 20'000'000 ||
            initial.pacing_bitrate_bps != 40'000'000 ||
            initial.pacing_interval.count() != 2) {
            throw std::runtime_error("controller did not honor the requested initial rate");
        }

        remoe::AdaptiveStreamController::NetworkFeedback severe;
        severe.loss_fraction = 0.08;
        controller.observe_network(severe);
        auto decrease = controller.take_decision();
        if (!decrease || decrease->media_bitrate_bps != 17'000'000 ||
            decrease->force_key_frame) {
            throw std::runtime_error("severe loss did not reduce bitrate without an IDR burst");
        }

        remoe::AdaptiveStreamController::LocalFeedback local;
        local.scheduler_lateness_ms = 4.5;
        controller.observe_local(local);
        auto high_rate_lateness = controller.take_decision();
        if (high_rate_lateness && high_rate_lateness->pacing_interval.count() == 5) {
            throw std::runtime_error("high bitrate incorrectly selected a 5 ms burst interval");
        }

        remoe::AdaptiveStreamController overflow_controller(20'000'000);
        local.dropped_batches = 1;
        overflow_controller.observe_local(local);
        auto overflow = overflow_controller.take_decision();
        if (!overflow || overflow->pacing_interval.count() > 3 || !overflow->force_key_frame) {
            throw std::runtime_error("local pacing pressure was not applied");
        }

        controller.observe_network(severe);
        if (controller.take_decision()) {
            throw std::runtime_error("repeated feedback bypassed the bitrate cooldown");
        }

        remoe::AdaptiveStreamController low_rate_controller(6'000'000);
        remoe::AdaptiveStreamController::LocalFeedback late_scheduler;
        late_scheduler.scheduler_lateness_ms = 4.5;
        low_rate_controller.observe_local(late_scheduler);
        auto relaxed_interval = low_rate_controller.take_decision();
        if (!relaxed_interval || relaxed_interval->pacing_interval.count() != 5) {
            throw std::runtime_error("low bitrate did not relax an overloaded scheduler");
        }

        std::cout << "Adaptive stream controller test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Adaptive stream controller test failed: " << error.what() << '\n';
        return 1;
    }
}
