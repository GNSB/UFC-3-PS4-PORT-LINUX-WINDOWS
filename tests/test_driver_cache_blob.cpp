// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <vector>
#include "video_core/renderer_vulkan/vk_driver_cache_blob.h"

int main() {
    namespace Blob = Vulkan::DriverCacheBlob;
    std::array<std::uint8_t, 16> uuid{};
    uuid[3] = 42;
    std::vector<std::uint8_t> data(96, 0xab);
    const std::array<std::uint32_t, 4> header{32, 1, 0x10de, 0x1234};
    std::memcpy(data.data(), header.data(), sizeof(header));
    std::memcpy(data.data() + 16, uuid.data(), uuid.size());
    const auto hash = Blob::Checksum(data);
    const auto validate = [&](auto magic, auto checksum, const auto& bytes) {
        return Blob::Validate(magic, checksum, bytes, 0x10de, 0x1234, uuid);
    };
    assert(validate(Blob::Magic, hash, data).empty());
    // Empty, truncated (even with a recomputed checksum), damaged payload and bad envelope.
    assert(!validate(Blob::Magic, 0, std::vector<std::uint8_t>{}).empty());
    for (size_t size = 0; size < 32; ++size) {
        const std::span truncated{data.data(), size};
        assert(!validate(Blob::Magic, Blob::Checksum(truncated), truncated).empty());
    }
    auto damaged = data;
    damaged.back() ^= 1;
    assert(!validate(Blob::Magic, hash, damaged).empty());
    assert(!validate(0, hash, data).empty());
    // Structurally valid checksums must still reject the wrong GPU/UUID/header version/size.
    for (size_t offset : {size_t{0}, size_t{4}, size_t{8}, size_t{12}, size_t{19}}) {
        damaged = data;
        damaged[offset] ^= 1;
        assert(!validate(Blob::Magic, Blob::Checksum(damaged), damaged).empty());
    }
}
