#!/bin/bash
# Build the patched OpenAirInterface gNB and nrUE natively without NVIDIA GPU dependencies.

set -euo pipefail

usage() {
    echo "Usage: $0 [--install-deps] [openairinterface5g_dir]"
}

install_deps=0
oai_dir=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps)
            install_deps=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [ -n "$oai_dir" ]; then
                usage >&2
                exit 1
            fi
            oai_dir="$1"
            shift
            ;;
    esac
done

script_dir=$(realpath "$(dirname "${BASH_SOURCE[0]}")")
oai_root=$(realpath "${script_dir}/..")
oai_dir=$(realpath -sm "${oai_dir:-${oai_root}}")
results_dir="${RESULTS_DIR:-${oai_root}/results}"

if [ ! -x "${oai_dir}/cmake_targets/build_oai" ]; then
    echo "Error: OpenAirInterface source is missing at ${oai_dir}." >&2
    exit 1
fi

mkdir -p "$results_dir"
build_log="$results_dir/build-oai-native-$(date +%Y%m%d-%H%M%S).log"

export SIONNA_RK_CPU_ONLY=1
export CUDA_VISIBLE_DEVICES=""
export NVIDIA_VISIBLE_DEVICES=void

build_args=(
    -w SIMU
    --gNB
    --nrUE
    --cmake-opt -DENABLE_PLUGINS=ON
    # Keep the physical USRP/B200 driver enabled alongside the RF simulator.
    --cmake-opt -DOAI_USRP=ON
    --cmake-opt -DENABLE_CUDA=OFF
    --cmake-opt -DENABLE_DGX_OPTIMIZATIONS=OFF
    --cmake-opt -DENABLE_AVX2RVV=ON
    --cmake-opt -DAVX2RVV_ROOT=/home/ubuntu/avx2rvv
    --cmake-opt -DAVX2=OFF
    --cmake-opt -DAVX512=OFF
)

if [ "$install_deps" = "1" ]; then
    build_args=(-I "${build_args[@]}")
fi

echo "Building OpenAirInterface natively in CPU-only mode"
echo "OAI source: ${oai_dir}"
echo "Build log: ${build_log}"
{
"${oai_dir}/cmake_targets/build_oai" "${build_args[@]}"

# build_oai's SIMU target does not necessarily build the USRP module, even
# when OAI_USRP=ON. The B200 launcher loads this module as liboai_device.so.
build_dir="${oai_dir}/cmake_targets/ran_build/build"
echo "Building the B200/B210 USRP device plugin"
cmake --build "$build_dir" --target oai_usrpdevif -j "$(nproc)"
test -r "${build_dir}/liboai_device.so" || {
    echo "Error: ${build_dir}/liboai_device.so was not created." >&2
    exit 1
}
} 2>&1 | tee "$build_log"
