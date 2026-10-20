// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <limits>
#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vulkan/vulkan_format_traits.hpp>

namespace Vulkan::CopyBounds {

inline bool Range(u64 offset, u64 size, u64 capacity) {
    return offset <= capacity && size <= capacity - offset;
}

inline u32 MipSize(u32 base, u32 mip) { return mip < 32 ? std::max(base >> mip, 1u) : 1u; }

inline u64 Blocks(u64 size, u32 block) { return (size + block - 1) / block; }

inline bool ImageRegion(const vk::ImageCreateInfo& image, const vk::ImageSubresourceLayers& sub,
                        vk::Offset3D offset, vk::Extent3D extent, bool array_depth = false) {
    if (sub.mipLevel >= image.mipLevels || sub.mipLevel >= 32 || !sub.layerCount ||
        !Range(sub.baseArrayLayer, sub.layerCount, image.arrayLayers)) {
        return false;
    }
    const auto block = vk::blockExtent(image.format);
    const auto axis = [](s32 at, u32 size, u32 limit, u32 alignment) {
        return at >= 0 && size && u32(at) < limit && u32(at) % alignment == 0 &&
               Range(u32(at), size, Blocks(limit, alignment) * alignment);
    };
    return axis(offset.x, extent.width, MipSize(image.extent.width, sub.mipLevel), block[0]) &&
           axis(offset.y, extent.height, MipSize(image.extent.height, sub.mipLevel), block[1]) &&
           (array_depth && image.imageType != vk::ImageType::e3D
                ? offset.z == 0 && extent.depth == sub.layerCount
                : axis(offset.z, extent.depth, MipSize(image.extent.depth, sub.mipLevel),
                       block[2]));
}

// Saturate on arithmetic overflow so callers reject the region before barrier tracking too.
inline u64 BufferSize(const vk::BufferImageCopy& copy, vk::Format format) {
    const auto block = vk::blockExtent(format);
    u64 bytes = vk::blockSize(format);
    if (copy.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eStencil) {
        bytes = 1;
    } else if (copy.imageSubresource.aspectMask == vk::ImageAspectFlagBits::eDepth) {
        bytes = format == vk::Format::eD16Unorm || format == vk::Format::eD16UnormS8Uint ? 2 : 4;
    }
    const u64 width = Blocks(copy.imageExtent.width, block[0]);
    const u64 height = Blocks(copy.imageExtent.height, block[1]);
    const u64 rows =
        Blocks(copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width, block[0]);
    const u64 lines =
        Blocks(copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height, block[1]);
    const u64 slices = u64(copy.imageExtent.depth) * copy.imageSubresource.layerCount;
    constexpr u64 max = std::numeric_limits<u64>::max();
    if (!width || !height || !slices || rows < width || lines < height)
        return max;
    // bytes * (((slices - 1) * lines + height - 1) * rows + width)
    u64 size = slices - 1;
    const auto append = [&](u64 factor, u64 add) {
        if (size > (max - add) / factor)
            return false;
        size = size * factor + add;
        return true;
    };
    if (!append(lines, height - 1) || !append(rows, width) || !append(bytes, 0))
        return max;
    return size;
}

inline bool BufferImage(const vk::ImageCreateInfo& image, u64 capacity,
                        const vk::BufferImageCopy& copy) {
    return BufferSize(copy, image.format) != std::numeric_limits<u64>::max() &&
           ImageRegion(image, copy.imageSubresource, copy.imageOffset, copy.imageExtent) &&
           image.samples == vk::SampleCountFlagBits::e1 &&
           (!copy.bufferRowLength ||
            copy.bufferRowLength % vk::blockExtent(image.format)[0] == 0) &&
           (!copy.bufferImageHeight ||
            copy.bufferImageHeight % vk::blockExtent(image.format)[1] == 0) &&
           Range(copy.bufferOffset, BufferSize(copy, image.format), capacity);
}

// The dispatch walks the concatenated mip sizes, while tiled accesses use mip offsets.
template <typename Mip>
inline bool TilingLayout(std::span<const Mip> mips, u32 levels, u64 guest_size) {
    if (!levels || levels > mips.size() || !guest_size)
        return false;
    u64 linear_size = 0;
    for (u32 mip = 0; mip < levels; ++mip) {
        const auto& layout = mips[mip];
        if (!layout.size || !layout.pitch || !layout.height ||
            !Range(layout.offset, layout.size, guest_size))
            return false;
        linear_size += layout.size;
    }
    return linear_size == guest_size;
}

inline bool ImageCopy(const vk::ImageCreateInfo& src, const vk::ImageCreateInfo& dst,
                      const vk::ImageCopy& copy) {
    auto dst_extent = copy.extent;
    const auto sb = vk::blockExtent(src.format);
    const auto db = vk::blockExtent(dst.format);
    const u64 w = Blocks(copy.extent.width, sb[0]) * db[0];
    const u64 h = Blocks(copy.extent.height, sb[1]) * db[1];
    if (w > UINT32_MAX || h > UINT32_MAX)
        return false;
    dst_extent.width = u32(w);
    dst_extent.height = u32(h);
    const bool cross_volume =
        (src.imageType == vk::ImageType::e3D) != (dst.imageType == vk::ImageType::e3D);
    return ImageRegion(src, copy.srcSubresource, copy.srcOffset, copy.extent, cross_volume) &&
           ImageRegion(dst, copy.dstSubresource, copy.dstOffset, dst_extent, cross_volume);
}

} // namespace Vulkan::CopyBounds
