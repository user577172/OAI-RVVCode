#!/bin/bash
# Cross-compile the patched OpenAirInterface gNB and nrUE for RV64 Linux.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-oai-native.sh [options] [openairinterface5g_dir]

Options:
  --install-deps           Install OAI host build dependencies first.
  --toolchain-prefix PATH  Compiler prefix (default: ~/riscv-toolchain/opt/riscv-gcc16/bin/riscv64-unknown-linux-gnu-).
  --avx2rvv-root PATH      Directory containing sse2rvv.h and avx2rvv.h.
  --package-only           Skip compilation and rebuild the deployable bundle.
  --build-fpga-image       Build an XSAI gcpt.bin containing the OAI bundle.
  --xsai-env PATH          xsai-env checkout used for the FPGA image.
  --nexst-dir PATH         Copy the final gcpt.bin and bundle into this NEXST checkout.
  --with-usrp              Build the B200/B210 UHD plugin (requires RISCV_UHD_PREFIX).
  -h, --help               Show this help.

Environment overrides:
  RISCV_TOOLCHAIN_PREFIX, RISCV_SYSROOT, RISCV_LIBCONFIG_PREFIX,
  RISCV_OPENSSL_PREFIX, RISCV_OPENBLAS_PREFIX, RISCV_LKSCTP_PREFIX,
  RISCV_SIMDE_PREFIX, RISCV_ZLIB_PREFIX,
  RISCV_UHD_PREFIX,
  RISCV_HOST_TOOLS_ROOT,
  AVX2RVV_ROOT, RESULTS_DIR, JOBS, FPGA_ARTIFACTS_DIR,
  FPGA_WORKLOAD (performance|pbch, default: performance),
  FPGA_PERF_SYNC_TIMEOUT (default: 1800),
  FPGA_PERF_GNB_READY_TIMEOUT (default: 900),
  FPGA_PERF_WARMUP_SECONDS (default: 20 after UE synchronization),
  FPGA_PERF_MEASUREMENT_SECONDS (default: 120 after warm-up),
  FPGA_PERF_HEARTBEAT_INTERVAL (default: 60),
  FPGA_CLEANUP_TIMEOUT (default: 15 guest seconds),
  XSAI_MEMORY_SIZE_HUMAN (default: 4GB),
  XSAI_DIRECT_MAP_MEM_SIZE_HUMAN (default: 512MB)
EOF
}

install_deps=0
package_only=0
build_fpga_image=0
with_usrp=0
oai_dir=""
default_toolchain_root="${HOME}/riscv-toolchain/opt/riscv-gcc16"
toolchain_prefix="${RISCV_TOOLCHAIN_PREFIX:-${default_toolchain_root}/bin/riscv64-unknown-linux-gnu-}"
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
riscv_uhd_prefix="${RISCV_UHD_PREFIX:-${HOME}/riscv-toolchain/riscv-libs/install/uhd}"
xsai_env_root="${XSAI_ENV_ROOT:-${HOME}/xsai-env}"
nexst_dir="${NEXST_DIR:-${HOME}/nexst}"
fpga_workload="${FPGA_WORKLOAD:-performance}"
fpga_perf_sync_timeout="${FPGA_PERF_SYNC_TIMEOUT:-1800}"
fpga_perf_gnb_ready_timeout="${FPGA_PERF_GNB_READY_TIMEOUT:-900}"
fpga_perf_warmup_seconds="${FPGA_PERF_WARMUP_SECONDS:-20}"
fpga_perf_measurement_seconds="${FPGA_PERF_MEASUREMENT_SECONDS:-120}"
fpga_perf_heartbeat_interval="${FPGA_PERF_HEARTBEAT_INTERVAL:-60}"
fpga_cleanup_timeout="${FPGA_CLEANUP_TIMEOUT:-15}"
xsai_memory_size_human="${XSAI_MEMORY_SIZE_HUMAN:-4GB}"
# The upstream 3000 MB DMA-pool default leaves only about 1 GB for Linux.
# OAI itself does not use this tensor pool, so retain 512 MB and make the
# remaining memory available to the embedded initramfs and OAI processes.
xsai_direct_map_mem_size_human="${XSAI_DIRECT_MAP_MEM_SIZE_HUMAN:-512MB}"
riscv_dependency_cflags="-I${simde_prefix}/include -I${openssl_prefix}/include -I${libconfig_prefix}/include -I${openblas_prefix}/include -I${lksctp_prefix}/include -I${zlib_prefix}/include"

[[ "$fpga_workload" == "performance" || "$fpga_workload" == "pbch" ]] || {
    echo "Error: FPGA_WORKLOAD must be performance or pbch" >&2
    exit 1
}
for value in "$fpga_perf_sync_timeout" "$fpga_perf_gnb_ready_timeout" \
             "$fpga_perf_measurement_seconds" "$fpga_perf_heartbeat_interval" \
             "$fpga_cleanup_timeout"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || {
        echo "Error: FPGA performance time values must be positive integers" >&2
        exit 1
    }
done
[[ "$fpga_perf_warmup_seconds" =~ ^[0-9]+$ ]] || {
    echo "Error: FPGA warm-up seconds must be a nonnegative integer" >&2
    exit 1
}

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
        --package-only)
            package_only=1
            shift
            ;;
        --build-fpga-image)
            build_fpga_image=1
            shift
            ;;
        --xsai-env)
            [[ $# -ge 2 ]] || { echo "Error: --xsai-env requires a value" >&2; exit 1; }
            xsai_env_root="$2"
            shift 2
            ;;
        --nexst-dir)
            [[ $# -ge 2 ]] || { echo "Error: --nexst-dir requires a value" >&2; exit 1; }
            nexst_dir="$2"
            shift 2
            ;;
        --with-usrp)
            with_usrp=1
            shift
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

package_fpga_artifacts() {
for required in nr-softmodem nr-uesoftmodem nr-cuup nr_pbchsim; do
    [[ -x "$cross_build_dir/$required" ]] || {
        echo "Error: missing RV64 build product: $cross_build_dir/$required" >&2
        exit 1
    }
done

artifacts_dir="${FPGA_ARTIFACTS_DIR:-${oai_root}/artifacts}"
package_dir="$artifacts_dir/oai-xsai-rv64"
rootfs_dir="$package_dir/rootfs"
bundle_dir="$rootfs_dir/opt/oai"
archive="$artifacts_dir/oai-xsai-rv64-rootfs.tar.gz"
initramfs_manifest="$package_dir/initramfs-oai-xsai.txt"
toolchain_root="${RISCV_TOOLCHAIN_ROOT:-$(dirname "$(dirname "$(command -v "$cc")")")}"

case "$package_dir" in
    "$oai_root"/artifacts/*|"${FPGA_ARTIFACTS_DIR:-/nonexistent}"/*) ;;
    *) echo "Error: unsafe FPGA package path: $package_dir" >&2; exit 1 ;;
esac

rm -rf "$package_dir"
mkdir -p "$bundle_dir/bin" "$bundle_dir/lib" "$bundle_dir/etc" \
    "$bundle_dir/scripts" "$rootfs_dir/etc" "$artifacts_dir"

install -m 755 \
    "$cross_build_dir/nr-softmodem" \
    "$cross_build_dir/nr-uesoftmodem" \
    "$cross_build_dir/nr-cuup" \
    "$cross_build_dir/nr_pbchsim" \
    "$bundle_dir/bin/"
install -m 755 "$oai_dir/scripts/run-k3-b200.sh" "$bundle_dir/scripts/"
install -m 755 "$oai_dir/scripts/run-xsai-fpga-performance.sh" "$bundle_dir/scripts/"
install -m 644 "$oai_dir/b210/gnb.sa.band78.24prbs.conf" "$bundle_dir/etc/"
install -m 644 "$oai_dir/b210/.env.example" "$bundle_dir/etc/b200.env.example"
install -m 644 \
    "$oai_dir/ci-scripts/conf_files/gnb.sa.band78.24prb.rfsim.conf" \
    "$bundle_dir/etc/gnb.sa.band78.24prb.rfsim.conf"
install -m 644 \
    "$oai_dir/targets/PROJECTS/GENERIC-NR-5GC/CONF/ue.conf" \
    "$bundle_dir/etc/ue.conf"
install -m 644 \
    "$oai_dir/targets/PROJECTS/GENERIC-NR-5GC/CONF/channelmod_rfsimu_LEO_satellite.conf" \
    "$bundle_dir/etc/channelmod_rfsimu_LEO_satellite.conf"
printf '%s\n' "$fpga_workload" >"$bundle_dir/etc/fpga-workload"
cat >"$bundle_dir/etc/fpga-performance.conf" <<EOF
SYNC_TIMEOUT=$fpga_perf_sync_timeout
GNB_READY_TIMEOUT=$fpga_perf_gnb_ready_timeout
WARMUP_SECONDS=$fpga_perf_warmup_seconds
MEASUREMENT_SECONDS=$fpga_perf_measurement_seconds
FRAME_MARKER_INTERVAL=128
HEARTBEAT_INTERVAL=$fpga_perf_heartbeat_interval
CLEANUP_TIMEOUT=$fpga_cleanup_timeout
GNB_THREAD_POOL=-1
UE_THREAD_POOL=-1,-1
EOF

copy_shared_objects() {
    local source_dir="$1"
    [[ -d "$source_dir" ]] || return 0
    while IFS= read -r -d '' library; do
        cp -Lf "$library" "$bundle_dir/lib/$(basename "$library")"
    done < <(find "$source_dir" -maxdepth 1 \( -type f -o -type l \) \
        -name '*.so*' -print0)
}

copy_shared_objects "$cross_build_dir"
copy_shared_objects "$libconfig_prefix/lib"
copy_shared_objects "$openssl_prefix/lib"
copy_shared_objects "$openblas_prefix/lib"
copy_shared_objects "$lksctp_prefix/lib"
copy_shared_objects "$zlib_prefix/lib"
copy_shared_objects "$gcc_target_libdir"
copy_shared_objects "$sysroot/lib"
copy_shared_objects "$sysroot/usr/lib"
copy_shared_objects "$sysroot/usr/lib/$target_triple"
if [[ "$with_usrp" == "1" ]]; then
    copy_shared_objects "$riscv_uhd_prefix/lib"
fi

loader=$(find "$sysroot" -type f -name 'ld-linux-riscv64-lp64d.so.1' -print -quit)
[[ -n "$loader" ]] || {
    echo "Error: RISC-V dynamic loader was not found below $sysroot" >&2
    exit 1
}
install -m 755 "$loader" "$bundle_dir/lib/ld-linux-riscv64-lp64d.so.1"

cat >"$rootfs_dir/etc/init" <<'EOF'
#!/bin/busybox sh
set -u

/bin/busybox mount -t proc proc /proc 2>/dev/null || true
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null || true
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
/bin/busybox --install -s

export PATH=/opt/oai/bin:/opt/oai/scripts:/bin:/sbin:/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/opt/oai/lib
export OAI_BUNDLE_ROOT=/opt/oai

if [ -r /opt/oai/etc/fpga-performance.conf ]; then
    . /opt/oai/etc/fpga-performance.conf
    export WARMUP_SECONDS MEASUREMENT_SECONDS SYNC_TIMEOUT GNB_READY_TIMEOUT
    export FRAME_MARKER_INTERVAL
    export HEARTBEAT_INTERVAL GNB_THREAD_POOL UE_THREAD_POOL
    export CLEANUP_TIMEOUT
fi

echo "[oai-xsai] RV64 OAI FPGA image booted"
workload=$(cat /opt/oai/etc/fpga-workload 2>/dev/null || echo performance)
status=0
case "$workload" in
    performance)
        echo "[oai-xsai] starting ideal-channel RFsim performance experiment"
        /opt/oai/scripts/run-xsai-fpga-performance.sh 1 1 || status=$?
        ;;
    pbch)
        echo "[oai-xsai] starting PBCH simulation smoke test"
        /opt/oai/bin/nr_pbchsim -s20 -S21 -n1 -o8000 -I -R106 || status=$?
        ;;
    *)
        echo "[oai-xsai] unknown FPGA workload: $workload"
        status=2
        ;;
esac
result=FAIL
[ "$status" -eq 0 ] && result=PASS
count=0
while [ "$count" -lt 5 ]; do
    echo "[oai-xsai] OAI_XSAI_RESULT=$result workload=$workload profile=newwork status=$status"
    count=$((count + 1))
    sleep 1
done
sync
echo "[oai-xsai] workload finished; powering off"
poweroff -f
exec /bin/sh
EOF
chmod 755 "$rootfs_dir/etc/init"

{
    echo 'dir /opt 755 0 0'
    echo 'dir /opt/oai 755 0 0'
    echo 'dir /opt/oai/bin 755 0 0'
    echo 'dir /opt/oai/lib 755 0 0'
    echo 'dir /opt/oai/etc 755 0 0'
    echo 'dir /opt/oai/scripts 755 0 0'
    echo 'file /bin/busybox ${RISCV_ROOTFS_HOME}/rootfsimg/build/busybox 755 0 0'
    echo 'slink /bin/sh /bin/busybox 755 0 0'
    echo "file /etc/init $rootfs_dir/etc/init 755 0 0"
    echo 'slink /init /etc/init 755 0 0'
    while IFS= read -r source; do
        target=${source#"$rootfs_dir"}
        mode=644
        case "$target" in
            /opt/oai/bin/*|/opt/oai/lib/*|/opt/oai/scripts/*) mode=755 ;;
        esac
        printf 'file %s %s %s 0 0\n' "$target" "$source" "$mode"
    done < <(find "$bundle_dir" -type f | sort)
} >"$initramfs_manifest"

(cd "$package_dir" && find rootfs -type f -print0 | sort -z | xargs -0 sha256sum) \
    >"$package_dir/SHA256SUMS"
tar -C "$rootfs_dir" -czf "$archive" .

echo "RV64 OAI bundle:    $bundle_dir"
echo "Rootfs archive:     $archive"
echo "Initramfs manifest: $initramfs_manifest"

if [[ "$build_fpga_image" == "1" ]]; then
    [[ -f "$xsai_env_root/firmware/Makefile" ]] || {
        echo "Error: xsai-env firmware tree not found: $xsai_env_root" >&2
        exit 1
    }
    echo "Building XSAI Linux/GCPT image with embedded OAI bundle"
    export PATH="$toolchain_root/bin:$PATH"
    xsai_memory_args=(
        "XSAI_MEMORY_SIZE_HUMAN=$xsai_memory_size_human"
        "XSAI_DIRECT_MAP_MEM_SIZE_HUMAN=$xsai_direct_map_mem_size_human"
    )
    echo "XSAI guest RAM:     $xsai_memory_size_human"
    echo "XSAI DMA pool:      $xsai_direct_map_mem_size_human"
    make -C "$xsai_env_root/firmware" init \
        RISCV="$toolchain_root" "${xsai_memory_args[@]}"
    make -C "$xsai_env_root/firmware/riscv-rootfs" apps/busybox \
        RISCV="$toolchain_root" \
        RISCV_ROOTFS_HOME="$xsai_env_root/firmware/riscv-rootfs"
    make -C "$xsai_env_root/firmware" build-linux \
        RISCV="$toolchain_root" \
        "${xsai_memory_args[@]}" \
        INITRAMFS_SOURCE="$initramfs_manifest" \
        FORCE_LINUX_CONFIG=1 \
        LINUX_MAKE_JOBS="$jobs"
    if [[ "$fpga_workload" == "performance" ]]; then
        kernel_dir="$xsai_env_root/firmware/riscv-linux"
        echo "Enabling Linux loopback/TCP support for the OAI RFsim workload"
        (
            cd "$kernel_dir"
            ./scripts/config --enable NET
            ./scripts/config --enable UNIX
            ./scripts/config --enable INET
            ./scripts/config --enable NETDEVICES
        )
        make -C "$kernel_dir" \
            ARCH=riscv \
            CROSS_COMPILE="$toolchain_root/bin/riscv64-unknown-linux-gnu-" \
            olddefconfig
        make -C "$kernel_dir" -j"$jobs" \
            ARCH=riscv \
            CROSS_COMPILE="$toolchain_root/bin/riscv64-unknown-linux-gnu-" \
            Image
        grep -q '^CONFIG_NET=y' "$kernel_dir/.config" || {
            echo "Error: Linux networking was not enabled" >&2
            exit 1
        }
    fi
    make -C "$xsai_env_root/firmware" build-dtb-nemu \
        RISCV="$toolchain_root" "${xsai_memory_args[@]}" 2>/dev/null || \
        make -C "$xsai_env_root/firmware" build-dtb \
            RISCV="$toolchain_root" "${xsai_memory_args[@]}"
    make -C "$xsai_env_root/firmware" build-gcpt-nemu \
        RISCV="$toolchain_root" \
        "${xsai_memory_args[@]}" \
        INITRAMFS_SOURCE="$initramfs_manifest" \
        OPENSBI_MAKE_JOBS="$jobs" \
        GCPT_MAKE_JOBS="$jobs"

    gcpt_source="$xsai_env_root/firmware/gcpt_restore/build-nemu/build/gcpt.bin"
    [[ -f "$gcpt_source" ]] || {
        echo "Error: expected GCPT output was not created: $gcpt_source" >&2
        exit 1
    }
    install -m 644 "$gcpt_source" "$artifacts_dir/gcpt-oai-xsai.bin"
    (cd "$artifacts_dir" && sha256sum gcpt-oai-xsai.bin \
        >gcpt-oai-xsai.bin.sha256)
    echo "FPGA payload:       $artifacts_dir/gcpt-oai-xsai.bin"

    if [[ -d "$nexst_dir/.git" ]]; then
        install -m 644 "$artifacts_dir/gcpt-oai-xsai.bin" \
            "$nexst_dir/tmp/gcpt-oai-xsai.bin"
        install -m 644 "$artifacts_dir/gcpt-oai-xsai.bin.sha256" \
            "$nexst_dir/tmp/gcpt-oai-xsai.bin.sha256"
        echo "NEXST staging:      $nexst_dir/tmp/"
    fi
fi
}

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

usrp_cmake=OFF
optional_cmake_prefix=""
if [[ "$with_usrp" == "1" ]]; then
    if [[ ! -f "${riscv_uhd_prefix}/include/uhd/version.hpp" ]]; then
        echo "Error: --with-usrp requires a RISC-V UHD installation at ${riscv_uhd_prefix}." >&2
        echo "Install UHD for the target first or omit --with-usrp for RFsim/PBCH FPGA validation." >&2
        exit 1
    fi
    usrp_cmake=ON
    optional_cmake_prefix=";${riscv_uhd_prefix}"
    riscv_dependency_cflags+=" -I${riscv_uhd_prefix}/include"
    riscv_dependency_ldflags+=" -L${riscv_uhd_prefix}/lib"
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
echo "RISC-V UHD/B200:   ${riscv_uhd_prefix} (${usrp_cmake})"
echo "GCC target libs:   ${gcc_target_libdir}"
echo "AVX2RVV:           ${avx2rvv_root} (${enable_avx2rvv})"
echo "RV64 CFLAGS:       ${riscv_cflags}"
echo "Build log:         ${build_log}"

if [[ "$package_only" == "0" ]]; then
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
        -DCMAKE_PREFIX_PATH="$libconfig_prefix;$openssl_prefix;$openblas_prefix;$lksctp_prefix;$simde_prefix;$zlib_prefix$optional_cmake_prefix" \
        -DCMAKE_FIND_ROOT_PATH="$sysroot;$libconfig_prefix;$openssl_prefix;$openblas_prefix;$lksctp_prefix;$simde_prefix;$zlib_prefix$optional_cmake_prefix" \
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
        -DOAI_USRP="$usrp_cmake" \
        -DENABLE_CUDA=OFF \
        -DENABLE_DGX_OPTIMIZATIONS=OFF \
        -DENABLE_AVX2RVV="$enable_avx2rvv" \
        -DAVX2RVV_ROOT="$avx2rvv_root" \
        -DAVX2=OFF \
        -DAVX512=OFF

    build_targets=(
        nr-softmodem nr-cuup nr-uesoftmodem nr_pbchsim
        params_libconfig coding rfsimulator
    )
    if [[ "$with_usrp" == "1" ]]; then
        build_targets+=(oai_usrpdevif)
    fi
    cmake --build "$cross_build_dir" --target "${build_targets[@]}" -j "$jobs"

    file \
        "$cross_build_dir/nr-softmodem" \
        "$cross_build_dir/nr-uesoftmodem" \
        "$cross_build_dir/nr_pbchsim" \
        "$cross_build_dir/nr-cuup" \
        "$cross_build_dir/librfsimulator.so" \
        "$cross_build_dir/libparams_libconfig.so"
} 2>&1 | tee "$build_log"
fi

package_fpga_artifacts
