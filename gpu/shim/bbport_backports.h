// SPDX-License-Identifier: GPL-2.0-or-later
// ufc3 (UFC-89): switches for the shadPS4 upstream fixes backported after b4e7ae7 from other
// PS4 ports of the same family (gpu/VENDOR.txt), one per fix, so a fix that breaks something
// can be turned off without a rebuild. Read once from the environment:
//   BB_BP<n>=0/1      one fix off/on (n is the upstream pull request; BB_BP_RECONV for maximal
//                     reconvergence, which has no upstream number)
//   BB_BACKPORTS=0    every fix of this list off (the behaviour before UFC-89); a BB_BP<n> set
//                     at the same time still wins
// The states are printed once at start-up ("Backports: 5406=on ..."). Fixes that change the
// generated shaders are part of the shader binary version (ShaderBits), so binaries compiled
// with the other state are not reused.
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbBackport {

enum Id : unsigned {
    CeDeWaits5406,     ///< CE/DE counter waits: ce - de, no CE task to wait for
    IbFrameFree5258,   ///< the coroutine frame of a finished nested indirect buffer is freed
    IndexBuffer5358,   ///< null or unmapped index buffers (#5358, #5327)
    SamplerClamp5137,  ///< undefined S# fields clamped to defined values
    SubmitWaits5231,   ///< every submit wait covers all stages
    ImageRange5240,    ///< FindImageFromRange looks only at the first page
    InputBarriers5262, ///< vertex, index and indirect buffers take part in barriers
    FirstInstance5348, ///< vertex and instance ids unbiased in every indirect draw (shader)
    MaxReconvergence,  ///< VK_KHR_shader_maximal_reconvergence in compute (shader, device)
    EarlyZ5264,        ///< EarlyFragmentTests with DB_SHADER_CONTROL.DEPTH_BEFORE_SHADER (shader)
    Count,
};

struct Entry {
    const char* name; ///< in the start-up line
    const char* env;
    bool default_on;
    unsigned shader_bit; ///< bit in ShaderBits(), 0 if the generated shaders do not change
};

inline constexpr std::array<Entry, Count> kEntries{{
    {"5406", "BB_BP5406", true, 0},
    {"5258", "BB_BP5258", true, 0},
    {"5358", "BB_BP5358", true, 0},
    {"5137", "BB_BP5137", true, 0},
    {"5231", "BB_BP5231", true, 0},
    {"5240", "BB_BP5240", true, 0},
    {"5262", "BB_BP5262", true, 0},
    {"5348", "BB_BP5348", true, 1},
    {"reconv", "BB_BP_RECONV", false, 2},
    {"5264", "BB_BP5264", false, 4},
}};

namespace Detail {
inline bool Read(const char* env, bool fallback) {
    const char* v = std::getenv(env);
    if (!v || !v[0]) {
        return fallback;
    }
    return !(v[0] == '0' || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0);
}

inline const std::array<bool, Count>& States() {
    static const std::array<bool, Count> states = [] {
        const bool all = Read("BB_BACKPORTS", true);
        std::array<bool, Count> s{};
        for (unsigned i = 0; i < Count; ++i) {
            s[i] = Read(kEntries[i].env, all && kEntries[i].default_on);
        }
        return s;
    }();
    return states;
}
} // namespace Detail

inline bool On(Id id) {
    return Detail::States()[id];
}

/// The shader-changing fixes that are on, for the shader binary version (3 bits).
inline unsigned ShaderBits() {
    unsigned bits = 0;
    for (unsigned i = 0; i < Count; ++i) {
        if (Detail::States()[i]) {
            bits |= kEntries[i].shader_bit;
        }
    }
    return bits;
}

/// "Backports: 5406=on 5258=on ..." on stdout, once.
inline void LogOnce() {
    static const bool logged = [] {
        char line[256];
        int n = std::snprintf(line, sizeof(line), "Backports:");
        for (unsigned i = 0; i < Count && n > 0 && n < int(sizeof(line)); ++i) {
            n += std::snprintf(line + n, sizeof(line) - n, " %s=%s", kEntries[i].name,
                               Detail::States()[i] ? "on" : "off");
        }
        std::printf("%s (BB_BACKPORTS=0: all off; BB_BP<n>=0/1 per fix)\n", line);
        std::fflush(stdout);
        return true;
    }();
    (void)logged;
}

} // namespace BbBackport
