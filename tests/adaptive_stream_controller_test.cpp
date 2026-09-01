#include "adaptive_stream_controller.h"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        remoe::AdaptiveStreamController controller(20'000'000);
        const auto initial = controller.initial_decision();
        if (initial.media_bitrate_bps != 10'000'000 ||
            initial.pacing_bitrate_bps != 15'000'000 ||
            initial.pacing_interval.count() != 2) {
            throw std::runtime_error("unexpected conservative start decision");
        }

        for (int index = 0; index < 3; ++index) {
            remoe::AdaptiveStreamController::NetworkFeedback clean;
            clean.receiver_report = true;
            controller.observe_network(clean);
        }
        auto increase = controller.take_decision();
        if (!increase || increase->media_bitrate_bps != 10'500'000) {
            throw std::runtime_error("clean reports did not increase bitrate");
        }

        remoe::AdaptiveStreamController::NetworkFeedback severe;
        severe.loss_fraction = 0.08;
        controller.observe_network(severe);
        auto decrease = controller.take_decision();
        if (!decrease || decrease->media_bitrate_bps >= increase->media_bitrate_bps ||
            !decrease->force_key_frame) {
            throw std::runtime_error("severe loss did not reduce bitrate and recover");
        }

        remoe::AdaptiveStreamController::LocalFeedback local;
        local.scheduler_lateness_ms = 4.5;
        local.dropped_batches = 1;
        controller.observe_local(local);
        auto overflow = controller.take_decision();
        if (!overflow || overflow->pacing_interval.count() != 5 ||
            !overflow->force_key_frame) {
            throw std::runtime_error("local pacing pressure was not applied");
        }

        std::cout << "Adaptive stream controller test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Adaptive stream controller test failed: " << error.what() << '\n';
        return 1;
    }
}
