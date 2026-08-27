#pragma once

#include <cstdint>
#include <vector>

namespace remoe {

struct EncodedVideoFrame {
    std::vector<std::uint8_t> data;
    bool key_frame = false;
};

} // namespace remoe

