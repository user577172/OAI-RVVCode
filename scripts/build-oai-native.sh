#!/bin/bash
# Cross-compile the patched OpenAirInterface gNB and nrUE for RV64 Linux.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-oai-native.sh [options] [openairinterface5g_dir]

Options:
  --install-deps           Install OAI host build dependencies first.
  --toolchain-prefix PATH  Compiler prefix (default: riscv64-unknown-linux-gnu-).
  --avx2rvv-root PATH      Directory containing sse2rvv.h and avx2rvv.h.
  -h, --help               Show this help.

Environment overrides:
  RISCV_TOOLCHAIN_PREFIX, RISCV_SYSROOT, RISCV_LIBCONFIG_PREFIX,
  RISCV_OPENSSL_PREFIX, RISCV_OPENBLAS_PREFIX, RISCV_LKSCTP_PREFIX,
  RISCV_SIMDE_PREFIX, RISCV_ZLIB_PREFIX,
  RISCV_HOST_TOOLS_ROOT,
  AVX2RVV_ROOT, RESULTS_DIR, JOBS
EOF
}

install_deps=0
oai_dir=""
toolchain_prefix="${RISCV_TOOLCHAIN_PREFIX:-riscv64-unknown-linux-gnu-}"
avx2rvv_root="${AVX2RVV_ROOT:-}"
enable_avx2rvv="${ENABLE_AVX2RVV:-OFF}"
riscv_cflags="${RISCV_CFLAGS:--march=rv64gcv_zba -mabi=lp64d}"
riscv_cxxflags="${RISCV_CXXFLAGS:-$riscv_cflags}"
libconfig_prefix="${RISCV_LIBCONFIG_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/libconfig}"
openssl_prefix="${RISCV_OPENSSL_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/openssl}"
openblas_prefix="${RISCV_OPENBLAS_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/openblas}"
lksctp_prefix="${RISCV_LKSCTP_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/lksctp}"
simde_prefix="${RISCV_SIMDE_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/simde}"
zlib_prefix="${RISCV_ZLIB_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/zlib}"
host_tools_root="${RISCV_HOST_TOOLS_ROOT:-${HOME}/riscv-toolchain/host-tools}"
riscv_dependency_cflags="-I${simde_prefix}/include -I${openssl_prefix}/include -I${libconfig_prefix}/include -I${openblas_prefix}/include -I${lksctp_prefix}/include -I${zlib_prefix}/include"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps)
            install_deps=1
            shift
            ;;
        --toolchain-prefix)
            [[ $# -ge 2 ]] || { echo "Error: --toolchain-prefix requires a value" >&2; exit 1; }
            toolchain_prefix="$2"
            shift 2
            ;;
        --avx2rvv-root)
            [[ $# -ge 2 ]] || { echo "Error: --avx2rvv-root requires a value" >&2; exit 1; }
            avx2rvv_root="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            if [[ -n "$oai_dir" ]]; then
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
jobs="${JOBS:-$(nproc)}"

if [[ ! -x "${oai_dir}/cmake_targets/build_oai" ]]; then
    echo "Error: OpenAirInterface source is missing at ${oai_dir}." >&2
    exit 1
fi

cc="${toolchain_prefix}gcc"
cxx="${toolchain_prefix}g++"
ar="${toolchain_prefix}ar"
ranlib="${toolchain_prefix}ranlib"
strip="${toolchain_prefix}strip"
if [[ -x "${host_tools_root}/asn1c/bin/asn1c" ]]; then
    asn1c_exec="${host_tools_root}/asn1c/bin/asn1c"
else
    asn1c_exec="$(command -v asn1c || true)"
fi
if [[ -z "$asn1c_exec" || ! -x "$asn1c_exec" ]]; then
    echo "Error: asn1c was not found on PATH." >&2
    exit 1
fi

for tool in "$cc" "$cxx" "$ar" "$ranlib" "$strip"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Error: required RISC-V tool not found: $tool" >&2
        exit 1
    }
done

target_triple=$("$cc" -dumpmachine)
if [[ "$target_triple" != riscv64*-linux-gnu* ]]; then
    echo "Error: $cc targets '$target_triple', expected riscv64 Linux GNU." >&2
    exit 1
fi

sysroot="${RISCV_SYSROOT:-$("$cc" -print-sysroot)}"
if [[ -z "$sysroot" || ! -d "$sysroot" ]]; then
    echo "Error: compiler sysroot is unavailable: ${sysroot:-<empty>}" >&2
    exit 1
fi
libgfortran_path=$("$cc" -print-file-name=libgfortran.so)
if [[ "$libgfortran_path" == "libgfortran.so" || ! -f "$libgfortran_path" ]]; then
    echo "Error: GCC 16 RISC-V libgfortran was not found." >&2
    exit 1
fi
gcc_target_libdir=$(dirname "$libgfortran_path")
riscv_dependency_ldflags="-L${libconfig_prefix}/lib -L${openssl_prefix}/lib -L${openblas_prefix}/lib -L${lksctp_prefix}/lib -L${zlib_prefix}/lib -L${gcc_target_libdir} -Wl,-rpath-link,${gcc_target_libdir}"

if [[ ! -f "${libconfig_prefix}/lib/pkgconfig/libconfig.pc" ]]; then
    echo "Error: RISC-V libconfig was not found at ${libconfig_prefix}." >&2
    exit 1
fi
if [[ ! -f "${openssl_prefix}/lib/pkgconfig/openssl.pc" ]]; then
    echo "Error: RISC-V OpenSSL was not found at ${openssl_prefix}." >&2
    exit 1
fi
if [[ ! -f "${openblas_prefix}/lib/pkgconfig/blas.pc" || ! -f "${openblas_prefix}/lib/pkgconfig/lapacke.pc" ]]; then
    echo "Error: RISC-V BLAS/LAPACKE was not found at ${openblas_prefix}." >&2
    exit 1
fi
if [[ ! -f "${lksctp_prefix}/include/netinet/sctp.h" ]]; then
    echo "Error: RISC-V lksctp-tools was not found at ${lksctp_prefix}." >&2
    exit 1
fi
if [[ ! -f "${simde_prefix}/include/simde/simde-common.h" ]]; then
    echo "Error: SIMDe headers were not found at ${simde_prefix}." >&2
    exit 1
fi
if [[ ! -f "${zlib_prefix}/include/zlib.h" || ! -f "${zlib_prefix}/lib/libz.a" ]]; then
    echo "Error: RISC-V zlib was not found at ${zlib_prefix}." >&2
    exit 1
fi

if [[ -z "$avx2rvv_root" ]]; then
    for candidate in "${oai_dir}/../avx2rvv" "${HOME}/avx2rvv" /home/ubuntu/avx2rvv; do
        if [[ -f "$candidate/sse2rvv.h" && -f "$candidate/avx2rvv.h" ]]; then
            avx2rvv_root="$candidate"
            break
        fi
    done
fi
if [[ "$enable_avx2rvv" == "ON" ]]; then
    if [[ ! -f "${avx2rvv_root}/sse2rvv.h" || ! -f "${avx2rvv_root}/avx2rvv.h" ]]; then
        echo "Warning: AVX2RVV headers were not found; disabling AVX2RVV." >&2
        enable_avx2rvv=OFF
    fi
fi

mkdir -p "$results_dir"
build_log="$results_dir/build-oai-riscv-gcc16-$(date +%Y%m%d-%H%M%S).log"
native_build_dir="${oai_dir}/cmake_targets/ran_build-native/build"
cross_build_dir="${oai_dir}/cmake_targets/ran_build-riscv-gcc16/build"

export SIONNA_RK_CPU_ONLY=1
export CUDA_VISIBLE_DEVICES=""
export NVIDIA_VISIBLE_DEVICES=void

if [[ "$install_deps" == "1" ]]; then
    "${oai_dir}/cmake_targets/build_oai" -I --disable-hardware-dependency
fi

if command -v ninja >/dev/null 2>&1; then
    generator=(-GNinja)
else
    generator=()
fi

echo "Cross-compiling OpenAirInterface for RISC-V in CPU-only RFsim mode"
echo "OAI source:        ${oai_dir}"
echo "C compiler:       $(command -v "$cc")"
echo "Compiler version: $("$cc" -dumpfullversion -dumpversion)"
echo "Target:            ${target_triple}"
echo "Sysroot:           ${sysroot}"
echo "RISC-V libconfig:  ${libconfig_prefix}"
echo "RISC-V OpenSSL:    ${openssl_prefix}"
echo "RISC-V OpenBLAS:   ${openblas_prefix}"
echo "RISC-V lksctp:     ${lksctp_prefix}"
echo "SIMDe headers:     ${simde_prefix}"
echo "RISC-V zlib:       ${zlib_prefix}"
echo "GCC target libs:   ${gcc_target_libdir}"
echo "AVX2RVV:           ${avx2rvv_root} (${enable_avx2rvv})"
echo "RV64 CFLAGS:       ${riscv_cflags}"
echo "Build log:         ${build_log}"

{
    # OAI generates LDPC tables with helper programs that must execute on the
    # x86_64 build host, so build those tools before configuring RV64 targets.
    cmake -S "$oai_dir" -B "$native_build_dir" "${generator[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DASN1C_EXEC="$asn1c_exec" \
        -DENABLE_CUDA=OFF \
        -DENABLE_DGX_OPTIMIZATIONS=OFF \
        -DAVX2=OFF \
        -DAVX512=OFF
    cmake --build "$native_build_dir" --target ldpc_generators generate_T -j "$jobs"

    # Cross libraries are installed in standalone RISC-V prefixes. Leaving
    # PKG_CONFIG_SYSROOT_DIR set would incorrectly prepend the compiler
    # sysroot to its already absolute include and library paths.
    export PKG_CONFIG_SYSROOT_DIR=""
    export PKG_CONFIG_LIBDIR="${libconfig_prefix}/lib/pkgconfig:${openssl_prefix}/lib/pkgconfig:${openblas_prefix}/lib/pkgconfig:${lksctp_prefix}/lib/pkgconfig:${zlib_prefix}/lib/pkgconfig:${sysroot}/usr/lib/${target_triple}/pkgconfig:${sysroot}/usr/lib/pkgconfig:${sysroot}/usr/share/pkgconfig"

    cmake -S "$oai_dir" -B "$cross_build_dir" "${generator[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DASN1C_EXEC="$asn1c_exec" \
        -DCROSS_COMPILE=ON \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
        -DCMAKE_C_COMPILER="$(command -v "$cc")" \
        -DCMAKE_CXX_COMPILER="$(command -v "$cxx")" \
        -DCMAKE_C_FLAGS="$riscv_cflags $riscv_dependency_cflags" \
        -DCMAKE_CXX_FLAGS="$riscv_cxxflags $riscv_dependency_cflags" \
        -DCMAKE_EXE_LINKER_FLAGS="$riscv_dependency_ldflags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$riscv_dependency_ldflags" \
        -DCMAKE_MODULE_LINKER_FLAGS="$riscv_dependency_ldflags" \
        -DCMAKE_AR="$(command -v "$ar")" \
        -DCMAKE_RANLIB="$(command -v "$ranlib")" \
        -DCMAKE_STRIP="$(command -v "$strip")" \
        -DCMAKE_SYSROOT="$sysroot" \
        -DCMAKE_PREFIX_PATH="$libconfig_prefix;$openssl_prefix;$openblas_prefix;$lksctp_prefix;$simde_prefix;$zlib_prefix" \
        -DCMAKE_FIND_ROOT_PATH="$sysroot;$libconfig_prefix;$openssl_prefix;$openblas_prefix;$lksctp_prefix;$simde_prefix;$zlib_prefix" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DbnProc_gen_128_DIR="$native_build_dir" \
        -DbnProc_gen_avx2_DIR="$native_build_dir" \
        -DbnProc_gen_avx512_DIR="$native_build_dir" \
        -DcnProc_gen_128_DIR="$native_build_dir" \
        -DcnProc_gen_avx2_DIR="$native_build_dir" \
        -DcnProc_gen_avx512_DIR="$native_build_dir" \
        -Dgenids_DIR="$native_build_dir" \
        -D_check_vcd_DIR="$native_build_dir" \
        -DOAI_USRP=OFF \
        -DENABLE_CUDA=OFF \
        -DENABLE_DGX_OPTIMIZATIONS=OFF \
        -DENABLE_AVX2RVV="$enable_avx2rvv" \
        -DAVX2RVV_ROOT="$avx2rvv_root" \
        -DAVX2=OFF \
        -DAVX512=OFF

    cmake --build "$cross_build_dir" --target \
        nr-softmodem nr-cuup nr-uesoftmodem params_libconfig coding rfsimulator \
        -j "$jobs"

    file \
        "$cross_build_dir/nr-softmodem" \
        "$cross_build_dir/nr-uesoftmodem" \
        "$cross_build_dir/nr-cuup" \
        "$cross_build_dir/librfsimulator.so" \
        "$cross_build_dir/libparams_libconfig.so"
} 2>&1 | tee "$build_log"
