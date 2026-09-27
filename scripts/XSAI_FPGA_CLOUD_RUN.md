# OAI XSAI-FPGA 编译与云平台运行说明

本文对应以下两个分支：

- OpenAirInterface：`~/openairinterface5g` 的 `XSAI-FPGA`
- NEXST：`~/nexst` 的 `oai-xsai`

目标是用 RISC-V GCC 16 编译 OAI，把运行程序、依赖库和最小 Linux rootfs 打包进 XSAI 的 `gcpt` 启动镜像，再由 NEXST 云平台流水线加载到 VP1902 FPGA。当前默认镜像启动后自动执行一次 OAI `nr_pbchsim` 烟雾测试。

## 1. 本机依赖

默认目录如下：

| 组件 | 路径/说明 |
| --- | --- |
| RISC-V GCC 16 | `~/riscv-toolchain/opt/riscv-gcc16` |
| RISC-V sysroot | `~/riscv-toolchain/opt/riscv-gcc16/sysroot` |
| RISC-V 第三方库 | `~/riscv-toolchain/riscv-libs/install` |
| 主机端 asn1c 等工具 | `~/riscv-toolchain/host-tools`（缺失时从 `PATH` 查找） |
| XSAI 构建环境 | `~/xsai-env` |
| NEXST 云平台仓库 | `~/nexst` |

脚本使用的目标编译器前缀为：

```text
~/riscv-toolchain/opt/riscv-gcc16/bin/riscv64-unknown-linux-gnu-
```

目标库包括 `libconfig`、OpenSSL、OpenBLAS、lksctp、SIMDe 和 zlib。默认不启用 UHD，因此当前镜像适用于 FPGA 上的 OAI CPU/PHY 程序验证和 PBCH 测试，不代表已经具备 B200 射频收发能力。

## 2. 编译与打包操作

以下命令都需要在 OAI 根目录执行：

```bash
cd ~/openairinterface5g
git switch XSAI-FPGA
```

### 2.1 默认编译：生成 RV64 OAI 和 rootfs 包

首次编译或者 OAI 源码发生变化时执行：

```bash
JOBS=8 ./scripts/build-oai-native.sh
```

`./scripts//build-oai-native.sh` 中多写一个 `/` 也可以运行，但建议统一使用上面的单斜杠写法。

该命令会使用 RISC-V GCC 16 交叉编译以下主要程序：

```text
cmake_targets/ran_build-riscv-gcc16/build/nr-softmodem
cmake_targets/ran_build-riscv-gcc16/build/nr-uesoftmodem
cmake_targets/ran_build-riscv-gcc16/build/nr-cuup
cmake_targets/ran_build-riscv-gcc16/build/nr_pbchsim
```

同时会收集 OAI 插件和目标动态库，生成：

```text
artifacts/oai-xsai-rv64/
artifacts/oai-xsai-rv64-rootfs.tar.gz
results/build-oai-riscv-gcc16-<时间戳>.log
```

默认命令不会构建 Linux/OpenSBI/GCPT，不会生成 `gcpt-oai-xsai.bin`，也不会把文件复制到 `~/nexst/tmp/`。

### 2.2 完整构建：编译并生成 NEXST FPGA 镜像

需要重新编译 OAI，并生成可上传到 NEXST 的 GCPT 镜像时执行：

```bash
JOBS=8 ./scripts/build-oai-native.sh \
  --build-fpga-image \
  --xsai-env ~/xsai-env \
  --nexst-dir ~/nexst
```

该命令依次完成：RV64 OAI 编译、rootfs 整理、Linux/initramfs 构建、OpenSBI/GCPT 打包，并把 `gcpt-oai-xsai.bin` 和校验文件复制到 `~/nexst/tmp/`。

### 2.3 仅重新打包：复用已有 RV64 OAI

如果 RV64 可执行文件已经成功生成，OAI 源码没有变化，只需重做 rootfs 和 FPGA 镜像：

```bash
JOBS=8 ./scripts/build-oai-native.sh \
  --package-only \
  --build-fpga-image \
  --xsai-env ~/xsai-env \
  --nexst-dir ~/nexst
```

`--package-only` 会跳过 OAI 的 CMake 编译阶段；如果 `cmake_targets/ran_build-riscv-gcc16/build/` 中缺少所需可执行文件，该命令会报错，此时应改用 2.2 的完整构建命令。

### 2.4 XSAI 内存参数

脚本默认使用 4 GB 客体内存，并把 XSAI DMA 保留区设为 512 MB。上游默认的 3000 MB 保留区会使 Linux 只剩约 858 MB，内置 OAI 测试会被 OOM 杀死。OAI 不使用这块张量 DMA 池；如其他负载需要调整，可设置：

```bash
XSAI_MEMORY_SIZE_HUMAN=4GB \
XSAI_DIRECT_MAP_MEM_SIZE_HUMAN=512MB \
JOBS=8 ./scripts/build-oai-native.sh --package-only --build-fpga-image
```

### 2.5 B200/UHD 可选构建

默认构建不包含 RISC-V UHD。只有已经把目标架构 UHD 安装到 `~/riscv-toolchain/riscv-libs/install/uhd` 后，才使用：

```bash
JOBS=8 ./scripts/build-oai-native.sh --with-usrp
```

`--with-usrp` 只负责构建 B200/UHD 支持；如果还需要同时生成 FPGA GCPT 镜像，应把它与 2.2 中的参数一起使用。

## 3. 生成的产物

OAI 目录中生成：

- `artifacts/gcpt-oai-xsai.bin`：上传/提交到 NEXST 的 FPGA 启动镜像。
- `artifacts/gcpt-oai-xsai.bin.sha256`：镜像校验文件。
- `artifacts/oai-xsai-rv64-rootfs.tar.gz`：独立 rootfs 归档，供检查或其他 Linux rootfs 集成使用；云流水线不需要提交它。
- `artifacts/oai-xsai-rv64/rootfs/opt/oai/`：解包后的 OAI bundle。
- `artifacts/oai-xsai-rv64/initramfs-oai-xsai.txt`：Linux initramfs 清单。
- `results/build-xsai-fpga-image.log`：镜像构建日志。
- `results/fpga-image-nemu-smoke-fixed.log`：NEMU 整机验证日志。

脚本会把云平台需要的两个文件复制到：

```text
~/nexst/tmp/gcpt-oai-xsai.bin
~/nexst/tmp/gcpt-oai-xsai.bin.sha256
```

`.gitlab-ci.yml` 会先校验 SHA-256，然后用以下软件加载命令：

```bash
NO_CONSOLE=1 HOLD_RESET=1 bash load_and_run_qdma.sh \
  qdma41000 ../../tmp/bootrom.bin ../../tmp/gcpt-oai-xsai.bin
```

## 4. 提交 NEXST 的 `oai-xsai` 分支

先确认没有把无关文件带入提交：

```bash
cd ~/nexst
git switch oai-xsai
git status --short
(cd tmp && sha256sum -c gcpt-oai-xsai.bin.sha256)
```

本次云平台运行只需提交流水线配置、GCPT 镜像及校验文件：

```bash
git add .gitlab-ci.yml \
  tmp/gcpt-oai-xsai.bin \
  tmp/gcpt-oai-xsai.bin.sha256
git commit -m "run OAI XSAI image on FPGA"
git push origin oai-xsai
```

`gcpt-oai-xsai.bin` 约 109 MiB；若服务器拒绝普通 Git 大文件，请按该平台管理员给出的 Git LFS 或上传限制处理，不要把 rootfs tar 再加入提交。

## 5. 在云平台启动

1. 打开 `http://8.152.206.202` 并登录。
2. 进入仓库 `nexst-tool/nexst`。
3. 进入 **Build → Pipelines**，点击 **New pipeline**。
4. 分支选择 `oai-xsai`，再点击 **New pipeline**。
5. 流水线先使用 `tmp/system-xsai-min-v6.pdi` 配置 VP1902，然后校验并加载 `tmp/gcpt-oai-xsai.bin`。
6. Job 日志出现 Web terminal 地址后打开该链接，按平台页面提示释放复位并查看串口输出。
7. 成功时应看到：

```text
PBCH test OK
[oai-xsai] OAI_XSAI_RESULT=PASS
```

流水线最多运行 2 小时。验证结束后回到 Job 页面点击 **Cancel** 结束任务并释放平台资源。

当前只修改软件镜像，继续使用仓库已有的 `system-xsai-min-v6.pdi`。只有硬件设计发生变化时，才需要把新的 `.pdi` 放入 `tmp/` 并同步修改 `.gitlab-ci.yml` 中 `program.vp1902.sh` 的文件名。

## 6. NEMU 本地复核

重建后可在上传前运行：

```bash
cd ~/xsai-env
make run-nemu
```

NEMU 会启动镜像并自动运行 PBCH 测试。看到 `OAI_XSAI_RESULT=PASS` 后可以退出。NEMU 是指令级仿真，执行时间会明显长于 qemu-user。

也可只用 qemu-user 快速检查打包后的 RV64 程序：

```bash
cd ~/openairinterface5g
bundle=artifacts/oai-xsai-rv64/rootfs/opt/oai
LD_LIBRARY_PATH="$bundle/lib" qemu-riscv64 \
  -L ~/riscv-toolchain/opt/riscv-gcc16/sysroot \
  "$bundle/bin/nr_pbchsim" -s20 -S21 -n1 -o8000 -I -R106
```

## 7. `run-k3-b200.sh` 的用途

打包后的镜像中，脚本位于 `/opt/oai/scripts/run-k3-b200.sh`：

```bash
/opt/oai/scripts/run-k3-b200.sh pbch   # 运行 PBCH 烟雾测试
/opt/oai/scripts/run-k3-b200.sh check  # 检查 UHD、B200、AMF 地址和配置
/opt/oai/scripts/run-k3-b200.sh run    # 在无 systemd 的最小 Linux 中前台运行 gNB
/opt/oai/scripts/run-k3-b200.sh start  # 在有 systemd 的系统中创建服务运行 gNB
```

要实际连接 B200，必须先把 RISC-V 版 UHD 安装到 `~/riscv-toolchain/riscv-libs/install/uhd`，再加 `--with-usrp` 重编译；还需复制并填写 `/opt/oai/etc/b200.env.example`，并确保运行节点能访问 B200 USB 设备和外部 5G Core。当前云平台 FPGA 流程是否透传 B200 USB/网络资源，需要平台侧另行确认。

## 8. 已完成的验证

- RV64 OAI bundle 和 GCPT 镜像成功构建。
- GCPT SHA-256 校验通过。
- NEMU 中 Linux 可用内存约 3.4 GB，未再发生 OOM。
- NEMU 整机启动后 OAI PBCH 测试结果：CRC 0 错误、payload 0 错误、`PBCH test OK`、`OAI_XSAI_RESULT=PASS`。
