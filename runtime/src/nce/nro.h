#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx::nce {

// Validated NRO image. Segment offsets are preserved as image offsets.
// BSS is described separately and is not included in image.
struct NroImage {
    std::vector<uint8_t> image;
    std::array<uint8_t, 32> build_id{};

    uint32_t file_size = 0;
    uint32_t mod0_offset = 0;

    uint64_t text = 0;
    uint64_t text_size = 0;
    uint64_t ro = 0;
    uint64_t ro_size = 0;
    uint64_t data = 0;
    uint64_t data_size = 0;
    uint64_t bss_size = 0;
};

// Validate the NRO header and segment ranges, then construct an image
// with segment bytes at their declared offsets. Returns nullptr on failure.
std::unique_ptr<NroImage> parse_nro_bytes(
    const std::vector<uint8_t>& raw,
    const std::string& path,
    std::string& error);

}  // namespace nx::nce
