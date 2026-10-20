// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PORTHEON (XAndromedaXR/UFC-4-PS4-PORT) 44edf33.
// The loop guard (emit_spirv.cpp): a fragment shader whose loop never ends on its own (its
// condition is FragCoord.x < 1e30) is emitted with a per-loop iteration counter that leaves the
// loop after kLoopGuardLimit iterations. Writes loop-guard.spv for spirv-val and checks that the
// counter's comparison is there. With BB_SHADER_LOOP_GUARD=0 it checks that the guard is absent.
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"

// Host services libbbgpu links against that test_gpu_runtime_stubs.cpp lacks (the game provides
// them).
extern "C" {
uint32_t platform_thread_id(void) { return 1; }
void platform_thread_get_name(char* out, size_t size) { if (size) out[0] = 0; }
void platform_describe_address(uintptr_t, char* out, size_t size) { if (size) out[0] = 0; }
int platform_backtrace(void**, int) { return 0; }
int runtime_guest_copy_range(uintptr_t*, uintptr_t*) { return 0; }
void runtime_ui_guest_flip(void) {}
}

int main(int argc, char** argv) {
    using namespace Shader;
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(info.hw_stage, info.sw_stage);
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block entry(pool), header(pool), body(pool), cont(pool), merge(pool);
    info.loads.Set(IR::Attribute::FragCoord, 0);
    for (u32 c = 0; c < 4; ++c) info.stores.Set(IR::Attribute::RenderTarget0, c);
    IR::U1 cond;
    {
        IR::IREmitter ir(cont);
        cond = ir.FPLessThan(ir.GetAttribute(IR::Attribute::FragCoord, 0), ir.Imm32(1e30f));
    }
    {
        IR::IREmitter ir(merge);
        for (u32 c = 0; c < 4; ++c) ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(c == 3 ? 1.0f : 0.5f), c);
        ir.Epilogue();
    }
    IR::Program program(info);
    for (IR::Block* b : {&entry, &header, &body, &cont, &merge}) program.blocks.push_back(b);
    using T = IR::AbstractSyntaxNode::Type;
    IR::AbstractSyntaxNode n{};
    n = {}; n.type = T::Block; n.data.block = &entry; program.syntax_list.push_back(n);
    n = {}; n.type = T::Block; n.data.block = &header; program.syntax_list.push_back(n);
    n = {}; n.type = T::Loop; n.data.loop.body = &body; n.data.loop.continue_block = &cont; n.data.loop.merge = &merge;
    program.syntax_list.push_back(n);
    n = {}; n.type = T::Block; n.data.block = &body; program.syntax_list.push_back(n);
    n = {}; n.type = T::Block; n.data.block = &cont; program.syntax_list.push_back(n);
    n = {}; n.type = T::Repeat; n.data.repeat.cond = cond; n.data.repeat.loop_header = &header; n.data.repeat.merge = &merge;
    program.syntax_list.push_back(n);
    n = {}; n.type = T::Block; n.data.block = &merge; program.syntax_list.push_back(n);
    n = {}; n.type = T::Return; program.syntax_list.push_back(n);
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Backend::Bindings bindings{};
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    std::ofstream out(dir / "loop-guard.spv", std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
    if (!out) return 1;
    // The guard's constant (65536) must be in the module.
    bool limit = false;
    for (size_t i = 0; i + 3 < code.size(); ++i)
        if ((code[i] & 0xffff) == 43 /* OpConstant */ && code[i + 3] == 65536) limit = true;
    const bool on = Backend::SPIRV::LoopGuardEnabled();
    std::printf("loop-guard-test: guard %s, %zu words, limit constant %s\n", on ? "on" : "off (BB_SHADER_LOOP_GUARD=0)",
                code.size(), limit ? "present" : "absent");
    return limit == on ? 0 : 1;
}
