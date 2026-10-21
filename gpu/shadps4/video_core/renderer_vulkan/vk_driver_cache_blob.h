// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace Vulkan::DriverCacheBlob {
constexpr size_t MaxBytes = 256 * 1024 * 1024;
constexpr std::uint64_t Magic = 0x3142435033434655ull; // UFC3PCB1

inline std::uint64_t Checksum(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (auto b : bytes) {
        hash = (hash ^ b) * 1099511628211ull;
    }
    return hash;
}

// Check the complete envelope before letting the driver see even the Vulkan header.
inline std::string_view Validate(std::uint64_t magic, std::uint64_t checksum,
                                 std::span<const std::uint8_t> data, std::uint32_t vendor,
                                 std::uint32_t device, std::span<const std::uint8_t, 16> uuid) {
    if (data.size() < 32 || data.size() > MaxBytes) {
        return "invalid blob size";
    }
    if (magic != Magic || checksum != Checksum(data)) {
        return "invalid envelope/checksum (corrupt or truncated blob)";
    }
    // Vulkan header v1: four uint32s followed by a 16-byte pipelineCacheUUID.
    std::array<std::uint32_t, 4> header{};
    std::memcpy(header.data(), data.data(), sizeof(header));
    if (header[0] != 32 || header[1] != 1 || header[2] != vendor || header[3] != device ||
        std::memcmp(data.data() + 16, uuid.data(), uuid.size()) != 0) {
        return "Vulkan header/GPU/UUID mismatch";
    }
    return {};
}
} // namespace Vulkan::DriverCacheBlob
