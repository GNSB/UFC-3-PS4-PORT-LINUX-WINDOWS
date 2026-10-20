#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# One identity for VERSION and all compiled components; patched submodules do not mark it dirty.
version=$(git describe --tags --match 'ufc3-v*' --always 2>/dev/null || echo unknown)
if ! git diff --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null; then version="$version-dirty"; fi
# Keep Windows runtime builds under ufc3-build's machine-wide build lock too.
if [[ ${1:-} == --windows-runtime ]]; then
    exec bash scripts/build-windows.sh --runtime
fi
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
# SacredFire: header-only libraries the host lacks (magic_enum, VulkanMemoryAllocator, xbyak) come
# from tools/deps.sh into out/deps/prefix; CMake finds them through CMAKE_PREFIX_PATH.
if [[ ! -f out/deps/prefix/include/vk_mem_alloc.h ]]; then bash tools/deps.sh; fi
export CMAKE_PREFIX_PATH="$PWD/out/deps/prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# bbport 0.51 (issue 124): a checkout updated from an earlier version keeps that version's patch
# applied, and the new one applies neither way ("patch does not apply"): the submodule's tree
# holds nothing but these patches, so it is reset and they are applied again.
fsr_patches_apply() {
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$@" "$PWD/$patch" || return 1
        fi
    done
}
if ! fsr_patches_apply --check 2>/dev/null; then
    echo 'FSR-Vulkan: an earlier version of the port changed it; resetting it and patching again'
    git -C gpu/third_party/fsr-vulkan checkout -- .
fi
fsr_patches_apply
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_BUILD_VERSION="$version" -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
# The runtime and its Linux host services (src/platform; the Windows ones are platform_win.c).
runtime=(src/runtime*.c src/platform/platform_posix.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" "-DBB_BUILD_VERSION=\"$version\"" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/sacredfire
echo "Built $PWD/out/sacredfire"
printf '%s\n' "$version" > out/VERSION
echo "Version: $version (out/VERSION)"
# GPU check for run.sh (live_resolution=auto) and the launcher's gamepad list (--gamepads).
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
# Fast identity checks; --version returns before reserving memory or initializing the GPU.
if [[ ${1:-} == --test || ${1:-} == --version-test ]]; then
    "$CC" -std=c11 -O2 -Wall -Wextra -Werror -Isrc tests/test_build_version.c -o out/build-version-test
    out/build-version-test
    [[ $(out/sacredfire --version) == "$version" ]] || { echo 'Embedded game version differs from VERSION' >&2; exit 1; }
    python3 - "$version" <<'PYTEST'
import pathlib, sys
identity = b"BB_GAME_BUILD_VERSION=" + sys.argv[1].encode() + b"\0"
assert identity in pathlib.Path("out/sacredfire").read_bytes(), "Missing game identity marker"
assert sys.argv[1].encode() + b"\0" in pathlib.Path("out/gpu/libbbgpu.so").read_bytes(), "Missing GPU version"
print("Embedded game and GPU versions: OK")
PYTEST
fi
if [[ ${1:-} == --test ]]; then
    ninja -C out/gpu write-window-merge-test
    out/gpu/write-window-merge-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c src/platform/platform_posix.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    # portheon: master volume, 7.1 fold for stereo devices, soft limiter.
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_audio.c src/platform/platform_posix.c "${libraries[@]}" -lm -o out/audio-test
    out/audio-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c src/platform/platform_posix.c -o out/file-mods-test
    # Keep running the remaining checks after the known file-mods failure, but retain
    # its exit status so --test still reports the failure.
    file_mods_status=0
    out/file-mods-test || file_mods_status=$?
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    # ufc3 online (UFC-25): the deterministic rcp/rsqrt patch against its rule (--all: every pattern).
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_deterministic_fp.c src/runtime_fp.c src/platform/platform_posix.c -lZydis -lm -o out/deterministic-fp-test
    out/deterministic-fp-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_eventflag.c "${runtime[@]}" out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/eventflag-test
    out/eventflag-test
    # ufc3 online: the PS4 network ABI (without the local server) and the Blaze/EASO server.
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_online_net.c src/runtime_net.c -o out/online-net-test
    out/online-net-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc -Wl,--gc-sections -ffunction-sections tests/test_time_conversion.c -o out/time-conversion-test
    out/time-conversion-test
    python3 -m unittest discover -s tests -p test_online_server.py
    exit "$file_mods_status"
fi
