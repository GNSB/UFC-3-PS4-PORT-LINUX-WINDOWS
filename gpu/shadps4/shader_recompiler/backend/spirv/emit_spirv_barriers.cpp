// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
void MemoryBarrier(EmitContext& ctx, spv::Scope scope) {
    const auto semantics{
        spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::UniformMemory |
        spv::MemorySemanticsMask::WorkgroupMemory | spv::MemorySemanticsMask::AtomicCounterMemory |
        spv::MemorySemanticsMask::ImageMemory};
    ctx.OpMemoryBarrier(ctx.ConstU32(static_cast<u32>(scope)),
                        ctx.ConstU32(static_cast<u32>(semantics)));
}
} // Anonymous namespace

void EmitBarrier(EmitContext& ctx) {
    const auto execution{spv::Scope::Workgroup};
    spv::Scope memory;
    spv::MemorySemanticsMask memory_semantics;
    if (ctx.sw_stage == Shader::SwStage::TessellationControl) {
        memory = spv::Scope::Invocation;
        memory_semantics = spv::MemorySemanticsMask::MaskNone;
    } else {
        memory = spv::Scope::Workgroup;
        memory_semantics =
            spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::WorkgroupMemory;
    }
    ctx.OpControlBarrier(ctx.ConstU32(static_cast<u32>(execution)),
                         ctx.ConstU32(static_cast<u32>(memory)),
                         ctx.ConstU32(static_cast<u32>(memory_semantics)));
}

void EmitWorkgroupMemoryBarrier(EmitContext& ctx) {
    if (ctx.profile.lds_divergent_barriers) {
        // ufc4 (Intel, mancelli's 8034718c): keeps GCN's lockstep LDS steps in order inside a
        // subgroup (SharedMemoryBarrierPass puts these between LDS writes and reads in divergent
        // code). A memory barrier only orders one invocation's accesses; Intel may run a
        // 32-wide subgroup as two SIMD16 passes, so the lanes are synchronized with a
        // subgroup-scope control barrier, shared memory semantics only.
        const auto semantics{spv::MemorySemanticsMask::AcquireRelease |
                             spv::MemorySemanticsMask::WorkgroupMemory};
        ctx.OpControlBarrier(ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup)),
                             ctx.ConstU32(static_cast<u32>(spv::Scope::Workgroup)),
                             ctx.ConstU32(static_cast<u32>(semantics)));
        return;
    }
    MemoryBarrier(ctx, spv::Scope::Workgroup);
}

void EmitDeviceMemoryBarrier(EmitContext& ctx) {
    MemoryBarrier(ctx, spv::Scope::Device);
}

} // namespace Shader::Backend::SPIRV
