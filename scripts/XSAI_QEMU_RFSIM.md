# XSAI OAI RISC-V QEMU RFsim 仿真环境说明

本文说明 `XSAIsim` 分支中 OAI RF simulator（RFsim）实验所使用的 RISC-V 编译器、QEMU、目标库、目录结构和启动方法。文中的路径是当前虚拟机 `lh` 用户下的默认安装位置；启动脚本允许通过环境变量覆盖这些路径。

## 1. 仿真方式

本实验同时启动以下两个 RISC-V Linux 程序：

- `nr-softmodem`：gNB；
- `nr-uesoftmodem`：nrUE。

两个程序通过 RFsim 的 TCP 端口 `4043` 交换 IQ 样本。它们由 `qemu-riscv64` 在 x86_64 Ubuntu 主机上执行。

这里使用的是 **QEMU Linux-user 模式**，不是：

- `qemu-system-riscv64` 全系统虚拟机；
- NEMU；
- 香山 RTL 或 FPGA；
- 一个完整的 RISC-V rootfs 镜像。

QEMU Linux-user 模式负责执行 RISC-V 用户态指令，并把系统调用转交给 Ubuntu 主机内核。RISC-V sysroot 只提供动态加载器、glibc 和目标架构运行库。

## 2. 当前验证过的环境

| 项目 | 当前配置 | 用途 |
| --- | --- | --- |
| OAI 分支 | `XSAIsim` | RISC-V OAI 源码和 RFsim 实验脚本 |
| OAI 基准提交 | `47bd3cd2f4` | 本文编写时的分支基准，工作区另有未提交修改 |
| xsai-env 分支 | `master`，提交 `2dc0f07` | 提供并构建 QEMU，统一设置 RISC-V 环境变量 |
| QEMU | `qemu-riscv64 9.0.91` | 执行 RISC-V Linux 用户态程序 |
| 编译器 | `riscv64-unknown-linux-gnu-gcc 16.2.0` | 交叉编译 OAI，并提供 GCC 目标运行库 |
| target triple | `riscv64-unknown-linux-gnu` | 64 位 RISC-V GNU/Linux |
| ISA/ABI | `-march=rv64gcv_zba -mabi=lp64d` | RV64、向量扩展、Zba，双精度硬浮点 ABI |
| 动态加载器 | `/lib/ld-linux-riscv64-lp64d.so.1` | 从 RISC-V sysroot 加载目标动态库 |
| QEMU CPU | `max` | 覆盖 OAI 二进制所需的 RISC-V 扩展 |
| QEMU 地址空间 | `-R 32G` | 为 OAI PHY 的大块内存映射预留 guest 虚拟地址空间 |

`-R 32G` 预留的是虚拟地址空间，并不表示立即占用 32 GiB 物理内存。缺少这个选项时，gNB 可能在 PHY 初始化阶段报 `malloc16_clear()` 断言失败。

## 3. 目录结构

```text
/home/lh/
├── openairinterface5g/
│   ├── scripts/build-oai-native.sh
│   ├── scripts/run-k3-hotspot-experiment.sh
│   └── cmake_targets/ran_build-riscv-gcc16/build/
│       ├── nr-softmodem
│       ├── nr-uesoftmodem
│       ├── librfsimulator.so
│       └── libparams_libconfig.so
├── xsai-env/
│   ├── env.sh
│   └── qemu/build/qemu-riscv64
└── riscv-toolchain/
    ├── opt/riscv-gcc16/
    │   ├── bin/riscv64-unknown-linux-gnu-gcc
    │   ├── sysroot/
    │   └── riscv64-unknown-linux-gnu/lib/
    ├── riscv-libs/install/
    │   ├── libconfig/
    │   ├── lksctp/
    │   ├── openblas/
    │   ├── openssl/
    │   ├── simde/
    │   └── zlib/
    └── host-tools/
```

当前磁盘占用量大致为：QEMU 构建目录 `1.3 GiB`、GCC 16 工具链 `3.3 GiB`、RISC-V 第三方库 `105 MiB`、OAI RISC-V 构建目录 `2.7 GiB`。

## 4. 各组件的作用

### 4.1 qemu-riscv64

实际使用的程序为：

```text
/home/lh/xsai-env/qemu/build/qemu-riscv64
```

启动脚本对 gNB 和 UE 使用的核心参数为：

```bash
qemu-riscv64 \
  -R 32G \
  -cpu max \
  -L /home/lh/riscv-toolchain/opt/riscv-gcc16/sysroot \
  -E MALLOC_ARENA_MAX=1 \
  -E LD_LIBRARY_PATH=... \
  /path/to/nr-softmodem
```

- `-cpu max`：提供当前 OAI 二进制需要的 RISC-V 指令扩展；
- `-L`：指定 RISC-V sysroot；
- `-R 32G`：解决 QEMU user 下的大块虚拟内存映射问题；
- `-E`：给 RISC-V 目标进程设置环境变量。

### 4.2 riscv64-unknown-linux-gnu-gcc

工具链安装在：

```text
/home/lh/riscv-toolchain/opt/riscv-gcc16
```

它包含：

- GCC/G++ 交叉编译器；
- binutils；
- RISC-V glibc sysroot；
- `libgcc_s`、`libstdc++` 和 `libatomic` 等目标运行库。

启动脚本也会调用 GCC 查询 `libgcc` 所在目录，因此当前运行流程要求该工具链保持在位，即使 OAI 已经编译完成。

### 4.3 RISC-V 第三方库

这些库必须是 **RISC-V 版本**，不能错误使用主机 `/usr/lib/x86_64-linux-gnu` 中的 x86_64 库。

| 目录 | 作用 |
| --- | --- |
| `libconfig` 1.8.2 | 解析 gNB/UE `.conf` 配置 |
| `lksctp` 1.0.21 | SCTP 协议支持，gNB 会链接 `libsctp.so.1` |
| OpenSSL 3.0.22 | TLS/密码功能，程序会链接 `libssl.so.3` 和 `libcrypto.so.3` |
| OpenBLAS 0.3.34 | BLAS/LAPACK 数值计算，主要用于构建阶段 |
| SIMDe 0.8.4 | 在非 x86 架构上兼容部分 SIMD intrinsic |
| zlib 1.3.1 | 压缩库依赖 |

### 4.4 主机工具

以下程序运行在 x86_64 Ubuntu 主机上，不是 RISC-V 目标程序：

- CMake、Make、ccache、pkg-config；
- `asn1c`；
- Git 和常用 shell 工具；
- systemd、sudo、`ss`。

`asn1c` 用于生成/构建 ASN.1 代码。若 RISC-V OAI 已完整编译，它不是每次启动 RFsim 都要执行的程序。

## 5. 为什么需要修改 OAI 源码

当前 OAI 基准代码不能直接作为 `riscv64-unknown-linux-gnu` 目标完成编译和 QEMU RFsim 运行，主要有以下原因：

1. OAI 的交叉编译分支原先把所有 `CROSS_COMPILE` 目标按 AArch64 处理，会加入 `-march=armv8-a` 等 ARM 参数；RISC-V GCC 无法接受这些参数。
2. 部分 LTE/NR PHY 代码通过 x86 架构宏选择 SIMD 实现。代码实际使用的是 SIMDe 可移植接口，但 RISC-V 默认不会进入该实现分支。
3. 当前源码树引用了可选的 plugin、neural receiver、neural demapper 和 MAC link-adaptation plugin 接口，但本项目不需要这些插件，且 RISC-V 构建环境中没有对应的可用目标库。
4. OAI 的 RVV DFT/IDFT 路径存在正确性问题：16 点快速路径未通过数值精度测试；2048 点 FFT 假定单次 RVV 操作可以固定处理 8 个数据块，在 QEMU 的 VLEN=128 环境中只会处理其中一部分。
5. 批量 IDFT 在第一个 OFDM symbol 添加循环前缀时，使用无符号表达式计算目标地址，`symbol == 0` 时可能发生地址下溢并导致段错误。

因此，这些修改首先是 **RISC-V 可编译性和运行正确性修复**，不是为了改变 5G 协议流程，也不是单纯追求更高性能。RFsim 仍使用 OAI 内置的标准 gNB、nrUE、调度、接收和解调路径。

## 6. OAI 源码修改内容

### 6.1 构建系统的 RISC-V 目标识别

修改文件：

- `CMakeLists.txt`

修改内容：

- 当 `CMAKE_SYSTEM_PROCESSOR` 为 `riscv64` 时，不再进入 AArch64 交叉编译分支；
- 不再向 RISC-V 编译器传递 `-march=armv8-a`；
- 当前 XSAI CPU-only RFsim 构建不编译 `plugins` 子目录。

### 6.2 移除本实验不使用的可选插件调用

修改文件：

- `executables/nr-ru.c`
- `executables/nr-softmodem.c`
- `executables/nr-uesoftmodem.c`
- `openair1/PHY/INIT/nr_init.c`
- `openair1/PHY/NR_TRANSPORT/nr_ulsch_demodulation.c`
- `openair1/PHY/NR_TRANSPORT/nr_ulsch_llr_computation.c`
- `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_primitives.c`

修改内容：

- 移除 plugin 初始化、线程 hook、释放函数和 channel-emulation plugin 调用；
- 移除 neural receiver 和 neural demapper 的分派接口，直接使用 OAI 内置的 PUSCH 接收、LLR 计算和 layer de-mapping 实现；
- 移除 MAC link-adaptation plugin 分派，使用 OAI 原有的内置 BLER/MCS 逻辑。

这些改动会关闭上述 **可选插件功能**，但不关闭 RFsim 自身，也不改变本实验使用的内置 CPU PHY 路径。若以后需要 neural receiver、外部 channel emulator 或自定义 link-adaptation plugin，需要为 RISC-V 单独移植对应插件，再恢复这些接口。

### 6.3 通过 SIMDe 使用可移植 SIMD 实现

修改文件：

- `openair1/PHY/CODING/3gpplte_turbo_decoder_sse_16bit.c`
- `openair1/PHY/CODING/3gpplte_turbo_decoder_sse_8bit.c`

修改内容：

- 在 RISC-V 下选择使用 SIMDe 类型和操作的兼容实现；
- 宏定义放在系统头文件之后，避免改变系统头文件的 ABI/架构选择。

这里不是让 RISC-V 执行真实的 x86 SSE 指令，而是让 SIMDe 把相应操作映射为目标平台可实现的代码。

### 6.4 修复 RVV DFT/IDFT 正确性问题

修改文件：

- `openair1/PHY/TOOLS/oai_dfts.c`
- `openair1/PHY/TOOLS/oai_dfts_rvv.h`

修改内容：

- 默认关闭未通过 DFT/IDFT 精度测试的实验性 16 点 RVV 快速路径，改用已验证的通用实现；只有显式定义 `OAI_ENABLE_EXPERIMENTAL_RVV_DFT16_FASTPATH` 才会重新启用该路径；
- 2048 点 RVV FFT/IDFT 根据运行时返回的实际 `vl` 分批处理数据，不再假定 `vl` 固定为 8；
- 修复批量 IDFT 的 symbol 输入/输出地址计算；
- 修复第一个 symbol 复制循环前缀时的无符号地址下溢。

2048 点修复仍保留 RVV 向量化。相对于原来的错误实现，它会完成全部数据块，所以执行工作量会增加，但输出才是有效的。16 点路径改用通用实现后可能有局部性能损失；循环前缀地址修复的额外开销可以忽略。

### 6.5 验证结果

修改后的代码已完成以下验证：

- RISC-V DFT/IDFT 数值测试通过；
- `nr_pbchsim` 完成 PBCH 解码，CRC 和 payload 错误均为 0；
- `nr-softmodem` 和 `nr-uesoftmodem` 均确认为 RISC-V 64 位 ELF；
- QEMU RFsim 理想信道实验中出现 `UE synchronized!`；
- gNB 和 nrUE 的测量窗口统计文件均产生有效调用增量；
- 未出现段错误、断言失败或 fatal error。

QEMU 中测得的耗时包含 TCG 动态翻译开销，不能直接用于评价香山 FPGA 或真实硬件性能。需要进行性能对比时，应使用相同源码和参数，在修改前后的 **正确实现**、NEMU 以及 FPGA 环境中分别采样。

## 7. 环境变量

推荐进入 `xsai-env` 后加载环境：

```bash
cd ~/xsai-env
source env.sh
```

当前 `env.sh` 会永久设置：

```bash
RISCV=~/riscv-toolchain/opt/riscv-gcc16
RISCV_SYSROOT=~/riscv-toolchain/opt/riscv-gcc16/sysroot
QEMU_LD_PREFIX=$RISCV_SYSROOT
PATH=$RISCV/bin:$PATH
```

实验启动脚本自身也提供默认路径，因此正常情况下不加载 `env.sh` 也能找到当前安装。加载它有助于手动检查和重新编译。

可覆盖的主要参数如下：

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `OAI_BUILD_DIR` | `cmake_targets/ran_build-riscv-gcc16/build` | RISC-V OAI 构建目录 |
| `XSAI_ENV_ROOT` | `~/xsai-env` | xsai-env 根目录 |
| `RISCV` | `~/riscv-toolchain/opt/riscv-gcc16` | GCC 工具链根目录 |
| `RISCV_SYSROOT` | `$RISCV/sysroot` | RISC-V sysroot |
| `RISCV_LIBS_ROOT` | `~/riscv-toolchain/riscv-libs/install` | RISC-V 第三方库根目录 |
| `QEMU_RISCV64` | `~/xsai-env/qemu/build/qemu-riscv64` | QEMU user 可执行文件 |
| `QEMU_CPU_MODEL` | `max` | QEMU RISC-V CPU 模型 |
| `QEMU_RESERVED_VA` | `32G` | guest 虚拟地址预留量 |
| `CPU_SET` | `0-7` | systemd 为两个进程允许使用的主机 CPU |
| `GNB_START_TIMEOUT` | `120` | 等待 gNB RFsim 端口的秒数 |
| `SYNC_TIMEOUT` | `600` | 每次等待 UE 同步的秒数 |
| `SYNC_ATTEMPTS` | `1` | UE 同步尝试次数 |
| `WARMUP_SECONDS` | `20` | 同步后的预热时间 |
| `MEASURE_SECONDS` | `120` | 统计窗口的墙钟时间 |
| `STATS_TIMEOUT` | `180` | 等待统计文件完整写出的秒数 |
| `RESULTS_DIR` | `~/openairinterface5g/results` | 实验结果目录 |

## 8. 启动前检查

```bash
cd ~/xsai-env
source env.sh

which riscv64-unknown-linux-gnu-gcc
riscv64-unknown-linux-gnu-gcc --version
~/xsai-env/qemu/build/qemu-riscv64 --version

test -d "$RISCV_SYSROOT"
test -x ~/xsai-env/qemu/build/qemu-riscv64

file ~/openairinterface5g/cmake_targets/ran_build-riscv-gcc16/build/nr-softmodem
file ~/openairinterface5g/cmake_targets/ran_build-riscv-gcc16/build/nr-uesoftmodem
```

两个 OAI 程序的 `file` 输出应包含 `ELF 64-bit`、`UCB RISC-V` 和 `interpreter /lib/ld-linux-riscv64-lp64d.so.1`。

## 9. 启动实验

运行一次理想信道实验：

```bash
cd ~/openairinterface5g
./scripts/run-k3-hotspot-experiment.sh ideal 1 1
```

参数含义：

```text
run-k3-hotspot-experiment.sh <模式> <重复次数> <起始编号>
```

- `ideal`：理想 RFsim 信道；
- `awgn`：AWGN 信道；
- `pair`：依次执行 ideal 和 AWGN；
- 第二个参数：重复次数；
- 第三个参数：结果编号起点。

例如，已有 `ideal-run1-*` 时不要覆盖旧结果，可以执行：

```bash
./scripts/run-k3-hotspot-experiment.sh ideal 1 2
```

如果 QEMU 下 UE 同步超过 600 秒：

```bash
SYNC_TIMEOUT=900 ./scripts/run-k3-hotspot-experiment.sh ideal 1 1
```

脚本会通过 sudo 创建临时 systemd service，设置 CPU 范围和资源限制，并在成功、失败或收到中断信号时停止 gNB/UE 服务。

## 10. 实验输出

默认结果位于：

```text
~/openairinterface5g/results
```

每轮实验主要产生：

- `*-gnb.log`：gNB 日志；
- `*-nrue.log`：UE 日志；
- `*-nrL1_stats.log` 和 `*-nrL1_UE_stats-0.log`：完整统计；
- `*-start-*.log`、`*-end-*.log`：测量窗口前后快照；
- `*-window-stats.csv`：窗口差分统计；
- `*-metadata.txt`：提交、二进制哈希、QEMU 版本和实验参数。

脚本不会覆盖同编号的已有结果，发现目标文件存在时会直接退出。

## 11. 重新编译 OAI

如果源码发生变化，可执行：

```bash
cd ~/xsai-env
source env.sh

cd ~/openairinterface5g
JOBS=2 bash scripts/build-oai-native.sh
```

默认编译参数为：

```text
-march=rv64gcv_zba -mabi=lp64d
```

构建脚本会检查 GCC、sysroot、libconfig、OpenSSL、OpenBLAS/LAPACKE、lksctp、SIMDe 和 zlib，并生成 `nr-softmodem`、`nr-uesoftmodem`、RFsim 插件等 RISC-V 产物。

## 12. 常见问题

### `Exec format error`

RISC-V ELF 被当作 x86_64 程序直接执行了。应使用已修改的启动脚本，让程序通过 `qemu-riscv64` 启动。

### `malloc16_clear()` 断言失败

通常是 QEMU user 的 guest 虚拟地址空间不足。确认启动参数中存在 `-R 32G`，不要随意删除 `QEMU_RESERVED_VA`。

### 找不到 `libssl.so.3`、`libsctp.so.1` 或 `libconfig.so`

检查 `RISCV_SYSROOT`、`RISCV_LIBS_ROOT` 和工具链是否仍位于本文所列目录。不要把 x86_64 动态库加入目标程序的库路径。

### UE 长时间没有 `UE synchronized!`

QEMU TCG 执行 106 PRB 的 OAI PHY 很慢。先查看 gNB 是否监听 `4043`，再查看 UE 日志是否出现 `Connection to 127.0.0.1:4043 established` 和 `Starting sync detection`。必要时提高 `SYNC_TIMEOUT`。

### 提示结果已经存在

第三个参数换成新的起始编号，不要删除或覆盖已有实验结果。

## 13. 使用限制

QEMU 适合验证：

- RISC-V 二进制能否加载；
- 依赖库是否完整；
- gNB/UE RFsim 能否连接；
- OAI 功能路径和 RISC-V 兼容性。

QEMU 下的热点耗时包含大量动态二进制翻译开销，不能直接代表香山处理器、NEMU 或 FPGA 的真实性能。需要进行香山硬件性能结论时，应在对应的 FPGA/硬件环境中重新采集数据。
