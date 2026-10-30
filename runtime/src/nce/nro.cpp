#include "nro.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nx::nce {

namespace {

template <typename T>
bool read_at(const std::vector<uint8_t>& bytes, uint64_t offset, T& out) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return false;

    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

struct Segment {
    uint32_t offset = 0;
    uint32_t size = 0;
};

bool segment_fits(const Segment& segment, uint32_t file_size) {
    return segment.offset <= file_size &&
           segment.size <= file_size - segment.offset;
}

}  // namespace

std::unique_ptr<NroImage> parse_nro_bytes(
    const std::vector<uint8_t>& raw,
    const std::string& path,
    std::string& error) {

    constexpr uint64_t kHeaderSize = 0x80;
    constexpr uint64_t kMagicOffset = 0x10;
    constexpr uint64_t kSizeOffset = 0x18;
    constexpr uint64_t kSegmentsOffset = 0x20;
    constexpr uint64_t kBssOffset = 0x38;
    constexpr uint64_t kBuildIdOffset = 0x40;

    auto fail = [&](const char* reason)
        -> std::unique_ptr<NroImage> {
        error = path + ": " + reason;
        return nullptr;
    };

    if (raw.size() < kHeaderSize)
        return fail("NRO is shorter than its header");

    if (std::memcmp(raw.data() + kMagicOffset, "NRO0", 4) != 0)
        return fail("invalid NRO magic");

    uint32_t declared_size = 0;
    if (!read_at(raw, kSizeOffset, declared_size))
        return fail("truncated NRO size field");

    if (declared_size < kHeaderSize)
        return fail("declared NRO size is smaller than its header");

    // A file may have an optional ASET section after the NRO itself.
    if (declared_size > raw.size())
        return fail("declared NRO size exceeds available file data");

    Segment segments[3]{};

    for (uint64_t i = 0; i < 3; ++i) {
        const uint64_t at = kSegmentsOffset + i * 8;

        if (!read_at(raw, at, segments[i].offset) ||
            !read_at(raw, at + 4, segments[i].size)) {
            return fail("truncated segment descriptor");
        }

        if (!segment_fits(segments[i], declared_size))
            return fail("segment extends beyond the declared NRO size");
    }

    if (segments[0].size == 0)
        return fail("text segment is empty");

    // Segment data must not overlap each other. The NRO header is expected
    // to reside within the text segment, so it is not treated separately.
    for (size_t i = 0; i < 3; ++i) {
        if (segments[i].size == 0)
            continue;

        const uint64_t a_begin = segments[i].offset;
        const uint64_t a_end = a_begin + segments[i].size;

        for (size_t j = i + 1; j < 3; ++j) {
            if (segments[j].size == 0)
                continue;

            const uint64_t b_begin = segments[j].offset;
            const uint64_t b_end = b_begin + segments[j].size;

            if (a_begin < b_end && b_begin < a_end)
                return fail("NRO segments overlap");
        }
    }

    uint32_t bss_size = 0;
    uint32_t mod0_offset = 0;

    if (!read_at(raw, kBssOffset, bss_size) ||
        !read_at(raw, 4, mod0_offset)) {
        return fail("truncated NRO start/header fields");
    }

    uint64_t image_size = 0;
    for (const Segment& segment : segments) {
        image_size = std::max(
            image_size,
            uint64_t(segment.offset) + segment.size);
    }

    if (image_size > std::numeric_limits<size_t>::max())
        return fail("NRO image is too large for this host");

    auto result = std::make_unique<NroImage>();
    result->file_size = declared_size;
    result->mod0_offset = mod0_offset;
    result->text = segments[0].offset;
    result->text_size = segments[0].size;
    result->ro = segments[1].offset;
    result->ro_size = segments[1].size;
    result->data = segments[2].offset;
    result->data_size = segments[2].size;
    result->bss_size = bss_size;

    std::memcpy(
        result->build_id.data(),
        raw.data() + kBuildIdOffset,
        result->build_id.size());

    result->image.assign(static_cast<size_t>(image_size), 0);

    for (const Segment& segment : segments) {
        if (segment.size == 0)
            continue;

        std::memcpy(
            result->image.data() + segment.offset,
            raw.data() + segment.offset,
            segment.size);
    }

        // MOD0 may reside in text, RO, or data. Validate it against the
    // complete reconstructed NRO image, not only the text segment.
    constexpr uint64_t kMod0HeaderSize = 0x1C;

    if ((mod0_offset & 3) != 0 ||
        mod0_offset > result->image.size() ||
        kMod0HeaderSize > result->image.size() - mod0_offset) {
        return fail("MOD0 header is misaligned or outside the NRO image");
    }

    if (std::memcmp(result->image.data() + mod0_offset, "MOD0", 4) != 0)
        return fail("MOD0 magic not found at the declared offset");

    error.clear();
    return result;
}

}  // namespace nx::nce
