# K3 + USRP B210配置目录

该目录保存K3原生OAI gNB连接B210及外部5G核心网所需的本机配置。启动入口仍兼容旧名称：

```bash
cd ~/sionna-rk/ext/openairinterface5g
./scripts/run-k3-b200.sh check
./scripts/run-k3-b200.sh start
```

这里的`b200`是OAI/UHD设备族的历史命名，当前实际设备可以是USRP B210。

## 文件

| 文件 | 用途 |
|---|---|
| `.env` | 当前K3的实际运行参数；默认不纳入Git |
| `.env.example` | 可提交的配置模板，新板可复制成`.env`后填写 |
| `gnb.sa.band78.24prbs.conf` | 24PRB、band 78的gNB基础模板 |
| `README.md` | 本说明及尚未完成的配置项目 |

## `.env`参数

```bash
USRP_SERIAL=31EC606
AMF_IP=核心网AMF地址
GNB_IP=K3用于N2/N3的本机地址
K3_GNB_BUILD=OAI构建目录
K3_GNB_ALLOWED_CPUS=允许gNB运行的CPU范围
K3_GNB_THREAD_POOL=可选的OAI线程池配置
K3_GNB_CONTINUOUS_TX=0
GNB_EXTRA_OPTIONS=
```

参数说明：

- `USRP_SERIAL`：`uhd_find_devices`显示的B210序列号。
- `AMF_IP`：gNB通过SCTP/N2连接的AMF IPv4地址。
- `GNB_IP`：K3上能够到达AMF、并用于N2/N3的地址；留空时脚本尝试通过路由自动选择。
- `K3_GNB_BUILD`：一般设为当前OAI的`cmake_targets/ran_build/build`。若目录移动，建议删除这一项，让脚本使用仓库内默认路径。
- `K3_GNB_ALLOWED_CPUS`：X100运行范围通常为`0-7`；不要在普通OAI进程中直接写A100范围。
- `K3_GNB_THREAD_POOL`：可留空使用OAI默认值。
- `K3_GNB_CONTINUOUS_TX`：正常真实UE接入保持`0`；连续发射只用于射频诊断。
- `GNB_EXTRA_OPTIONS`：传给`nr-softmodem`的额外参数。

## gNB模板中的关键参数

启动脚本不会直接修改模板，而是复制到：

```text
openairinterface5g/.runtime/k3-b200/gnb.conf
```

然后在运行时副本中写入：

- `tracking_area_code = 1`
- `MCC = 262`
- `MNC = 99`
- `.env`中的`AMF_IP`
- `.env`中的`GNB_IP`

模板还定义band 78、24PRB、SSB频点、Point A、TDD周期、RU和射频参数。

## 尚未包含在该目录中的部分

下面内容属于5G核心网，不是gNB/B210自身配置，目前还没有迁入：

- AMF、SMF和UPF的可执行程序或容器镜像；
- AMF/SMF/UPF配置；
- MySQL用户表和USIM鉴权数据；
- 核心网启动、状态和停止脚本；
- `tun0`、NAT和UE上网转发的永久化配置。

因此当前目录可以让OAI独立完成编译、RFsim实验和B210 gNB配置，但真实UE注册前仍必须先有可访问的AMF、SMF和UPF。

## 检查顺序

```bash
cd ~/sionna-rk/ext/openairinterface5g

# 1. 编译gNB、nrUE和USRP插件
./scripts/build-oai-native.sh

# 2. 确认B210、配置、二进制和AMF路由
./scripts/run-k3-b200.sh check

# 3. 确认核心网已经运行后启动gNB
./scripts/run-k3-b200.sh start

# 4. 查看状态和日志
./scripts/run-k3-b200.sh status
./scripts/run-k3-b200.sh log
```

日志保存在：

```text
openairinterface5g/results/k3-b200-optimization.log
openairinterface5g/results/k3-b200-gnb.log
```
