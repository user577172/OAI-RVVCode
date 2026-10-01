# XSAI-FPGA-NEWwork 云平台运行说明

OAI 分支 `XSAI-FPGA-NEWwork` 以 `XSAIsim` 为基准；NEXST 分支为 `oai-xsai`。OAI 采用 `~/riscv-toolchain/opt/riscv-gcc16` 的 RV64 交叉编译器，不编译 LLVM。

## 测量范围

启动 24 PRB、30 kHz SCS 的理想信道 RFsim gNB 和 nrUE。等待 UE 同步后**不预热**，立即按客体单调时钟测量 30 秒，然后停止工作负载。gNB/UE 的原始输出只进入匿名管道供状态解析，不保留完整日志；正常结束后，客体 `/results/oai-xsai-newwork/` 仅有 `oai-xsai-newwork.csv`。串口仅输出少量状态行和该 CSV 的内容。

CSV 包含客体测量秒数、CPU ticks 增量、观测到的帧 marker 和确认的帧数下界。FPGA 上 30 秒可能短于一个 128 帧 marker 的产生时间；此时 marker 数可为 0，帧率填 `NA`，**不代表吞吐量为 0**。这是一段短时间、低负载的运行/计数验证，不能代替长时间吞吐测试。

## 在虚拟机上构建

```bash
cd /home/lh/openairinterface5g
git switch XSAI-FPGA-NEWwork
JOBS=8 bash scripts/build-oai-native.sh --build-fpga-image \
  --xsai-env /home/lh/xsai-env --nexst-dir /home/lh/nexst
```

构建将 RV64 可执行程序打包到 `artifacts/gcpt-oai-xsai.bin`，并复制到 NEXST 的 `tmp/gcpt-oai-xsai.bin`。提交/推送 NEXST `oai-xsai` 分支时需一并包含镜像和 `.sha256` 文件。

## 云平台

在 NEXST GitLab 项目选择 `oai-xsai` 分支运行 `fpga_deploy`。该 Job 设置 12 小时超时，便于 FPGA 慢速启动和 UE 同步；**30 秒只指同步后的测量窗口**。Job 成功需要同时读到完整 CSV 和 `OAI_XSAI_RESULT=PASS`。

在 Job 页面下载 artifacts，文件为 `performance/oai-xsai-newwork.csv`，默认保留 30 天。若 Job 失败，先看少量状态行以及这个 CSV 是否存在；Job 日志不会包含完整 gNB/UE 输出。云端是否完成必须以 Job 实际状态为准，推送成功并不等于仿真成功。
