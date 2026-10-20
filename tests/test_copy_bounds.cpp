// SPDX-License-Identifier: GPL-2.0-or-later
// No Vulkan device: regression tests for the regions sent to the driver.
#include <array>
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_copy_bounds.h"

int main() {
    using namespace Vulkan::CopyBounds;
    vk::ImageCreateInfo image{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = vk::Extent3D{16, 8, 1},
        .mipLevels = 5,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
    };
    vk::BufferImageCopy c{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 2},
        .imageExtent = vk::Extent3D{16, 8, 1},
    };
    assert(BufferSize(c, image.format) == 1024);
    assert(BufferImage(image, 1024, c));
    assert(!BufferImage(image, 1023, c));
    c.bufferOffset = UINT64_MAX - 100;
    assert(!BufferImage(image, 1024, c));
    c.bufferOffset = 0;
    c.imageOffset.x = -1;
    assert(!BufferImage(image, 4096, c));
    c.imageOffset.x = 1;
    assert(!BufferImage(image, 4096, c));
    c.imageOffset.x = 0;
    c.imageSubresource.mipLevel = 32;
    assert(!BufferImage(image, 4096, c));
    c.imageSubresource.mipLevel = 0;
    c.imageSubresource.baseArrayLayer = UINT32_MAX;
    assert(!BufferImage(image, UINT64_MAX, c));
    c.imageSubresource.baseArrayLayer = 0;
    c.imageExtent.depth = 0;
    assert(!BufferImage(image, UINT64_MAX, c));
    c.imageExtent = vk::Extent3D{16, 8, 1};
    c.bufferRowLength = 32;
    c.bufferImageHeight = 16;
    assert(BufferSize(c, image.format) == (16 + 7) * 128 + 64);
    assert(!BufferImage(image, 2048, c));
    c.imageExtent = vk::Extent3D{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    c.imageSubresource.layerCount = UINT32_MAX;
    assert(BufferSize(c, image.format) == UINT64_MAX);

    // An otherwise valid array transfer must not wrap at the 32-bit byte boundary.
    auto large = image;
    large.extent = vk::Extent3D{4096, 4096, 1};
    large.arrayLayers = 65;
    const vk::BufferImageCopy array_copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 65},
        .imageExtent = {4096, 4096, 1},
    };
    constexpr u64 array_bytes = 4096ULL * 4096 * 4 * 65;
    assert(BufferSize(array_copy, large.format) == array_bytes);
    assert(BufferImage(large, array_bytes, array_copy));
    assert(!BufferImage(large, u32(array_bytes), array_copy));

    // Small BC mips still occupy a whole block, including implicit buffer row pitch.
    image.format = vk::Format::eBc1RgbaUnormBlock;
    image.extent = vk::Extent3D{7, 7, 1};
    c = {.imageSubresource = {vk::ImageAspectFlagBits::eColor, 2, 0, 1}, .imageExtent = vk::Extent3D{1, 1, 1}};
    assert(BufferSize(c, image.format) == 8);
    assert(BufferImage(image, 8, c));
    c.imageExtent = vk::Extent3D{8, 1, 1};
    assert(!BufferImage(image, 16, c));
    // Vulkan depth/stencil copies use the selected aspect's texel size.
    c.imageExtent = vk::Extent3D{1, 1, 1};
    c.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
    assert(BufferSize(c, vk::Format::eD32SfloatS8Uint) == 4);
    c.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
    assert(BufferSize(c, vk::Format::eD32SfloatS8Uint) == 1);

    vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = vk::Extent3D{7, 7, 1},
    };
    auto dst = image;
    dst.format = vk::Format::eR16G16B16A16Uint;
    dst.extent = vk::Extent3D{2, 2, 1};
    assert(ImageCopy(image, dst, copy)); // ceil(7/4) BC blocks -> 2 destination texels
    dst.extent.width = 1;
    assert(!ImageCopy(image, dst, copy));
    dst.extent.width = 2;
    copy.srcOffset.x = -4;
    assert(!ImageCopy(image, dst, copy));
    copy.srcOffset.x = 0;
    copy.dstSubresource.mipLevel = 5;
    assert(!ImageCopy(image, dst, copy));

    image.format = dst.format = vk::Format::eR8Unorm;
    image.extent = vk::Extent3D{8, 8, 1};
    image.arrayLayers = 4;
    dst = image;
    dst.imageType = vk::ImageType::e3D;
    dst.arrayLayers = 1;
    dst.extent.depth = 4;
    copy = {.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 4},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = vk::Extent3D{8, 8, 4}};
    assert(ImageCopy(image, dst, copy));
    assert(ImageCopy(dst, image,
                     vk::ImageCopy{copy.dstSubresource, {}, copy.srcSubresource, {}, copy.extent}));
    dst.extent.depth = 3;
    assert(!ImageCopy(image, dst, copy));
    dst.extent.depth = 4;
    copy.srcSubresource.baseArrayLayer = 1;
    assert(!ImageCopy(image, dst, copy));

    struct Mip {
        u32 size, pitch, height, offset;
    };
    std::array<Mip, 2> mips{{{512, 16, 8, 0}, {128, 8, 4, 512}}};
    const std::span<const Mip> layouts{mips};
    assert(TilingLayout(layouts, 2, 640));
    assert(!TilingLayout(layouts, 1, 640)); // shader would walk beyond supplied mips
    assert(!TilingLayout(layouts, 3, 640));
    mips[1].offset = 600;
    assert(!TilingLayout(layouts, 2, 640));
    assert(Range(64, 640, 704));
    assert(!Range(64, 640, 703));
    assert(!Range(UINT64_MAX, 640, 704));
    std::puts("copy bounds: OK");
}
