# BMRF-Gateway

**BMRF-Gateway**（BrandMeister RF Uplink Gateway）是一个面向 Android 的软件 DMR
网关/监视器，突出两大特性：

- **BM 网关**：作为 BrandMeister Homebrew 客户端接入 BM 网络，接收下行呼叫。
- **RF 上行接收**：通过 USB OTG + RTL-SDR 接收 DMR RF，将协议级信息上行注入 BM。

它不是完整 MMDVM hotspot。RF 侧只做接收，不做发射。

核心设计原则（见设计文档 §1.2 / §30）：**DMRFrame 是系统内部的唯一核心数据结构**，
不要把 AMBE/PCM 当作内部唯一表示。RF 与 BrandMeister 两个输入源都先归一化到统一的
`DMRFrame` / `DMRCall` / `DMREvent` 模型，再由 Call Manager 分发到 UI / BM 上行 / 音频链路。

```
RF (RTL-SDR) ─┐
              ├─► DMRFrame ─► Call Manager ─┬─► Dashboard UI
BM (Homebrew)─┘                            ├─► BM TX
                                           └─► JitterBuffer → AMBE → PCM → AAudio
```

## 下载

- 最新 APK 见 **[GitHub Releases](https://github.com/ShiJimmy/BMRF-Gateway/releases)**。
- 已用 release 密钥签名，可直接侧载安装（需允许"安装未知来源应用"）。
- 架构：`arm64-v8a` / `armeabi-v7a` / `x86_64`；最低 Android 8.0（API 26）。

## 当前实现范围

对应设计文档 §32 的 ①②③④，以及 BM 下行语音链路（MVP-2）：

- 统一数据模型：`DMRFrame` / `DMRCall` / `DMREvent`
- 线程安全事件总线 `EventBus`
- 呼叫状态机 `CallManager`（呼叫识别、开始/结束、帧计数、丢帧统计、超时、会话统计）
- BrandMeister Homebrew **RX** 解析（`DMRD` 55 字节、`MSTACK/NAK/PING/CL`、`RPTPONG`）
- BrandMeister Homebrew **编码**：默认使用 **原生 MMDVM 方言**
  （`RPTL`+4 字节 ID、`RPTK`+`SHA-256(salt||password)`、`RPTC`、`DMRD`、`RPTPING`，
  master 端口 **62031**）；go-brandmeister ASCII 方言
  （8 位十六进制 ID、`MSTACK`、`MSTPING`）保留在 `HomebrewEncoder` 供参考/测试
- 网络状态机 `HomebrewClient`（DISCONNECTED→CONNECTING→REGISTERED→ACTIVE→RECONNECTING）
- **下行语音管线**：`DMRFrame` → `JitterBuffer` → `IAmbeDecoder` → `IAudioOutput`，
  独立工作线程，Android 端使用 **AAudio**（8 kHz mono）
- **可插拔 AMBE 解码器**：默认 Null 解码器（静音），预留 `BMRF_USE_MBELIB` 开关
- JNI 桥接层（返回 JSON 状态 + 事件快照）
- **仪表盘 UI**：当前呼叫卡片、网络状态、会话统计（呼叫数/帧数/丢帧/丢帧率）、
  最近呼叫列表、音频状态与播放开关；前台服务承载核心
- **通讯录呼号查表**：在 UI 手动导入 CSV/TSV（只取前两列 `RADIO_ID,CALLSIGN`），
  通讯录中的 DMR ID 显示为 `ID 呼号`，否则只显示 ID（当前呼叫与历史列表均生效）
- 单元测试（Homebrew 解析、Call Manager、JitterBuffer/语音管线、上行成帧）与 `bmrf_frame_dump`

**RF 上行接收（协议层基础，stage RF-1）**
- `rf/dmr_sync`：DMR 48-bit 同步码分类（BS/MS × 语音/数据）
- `rf/fec`：clean-room Hamming(7,4)（CACH TACT）+ CRC-CCITT / 5-bit
- `rf/burst`：`DMRBurst`（132 dibit / 264 bit 突发模型 + 度量）
- `rf/rf_decoder`：突发 → `DMRFrame(source=Rf)` 语音路径（3×72bit AMBE →
  Homebrew 兼容 33B payload，复用 mbelib 解码）
- `rf/sample_source`：`ISampleSource`（cu8 IQ）+ Null 源
- `rf/burst_source`：`IDmrBurstSource` + 文件回放源（设计文档 §21）
- `rf/rf_receiver`：独立线程，突发源 → 解码 → CallManager / VoicePipeline / EventBus

**RF → BrandMeister 上行（stage RF-2 / MVP-3）**
- vendored DSDcc DMR 前端：RTL-SDR cu8 IQ → NCO/降采样 48k → FM 鉴频 → DMR 解码
- 空口嵌入 LC（经 FEC 校验）→ `DMRFrame`；暴露 LC 解码版本，新发射时等地址刷新去抖
- 上行成帧：Voice LC Header + 嵌入 LC 语音帧 + Terminator（原生方言 / 端口 62031）；
  通话组中途切换时先发 Terminator、延迟 200ms 再开新呼叫，避免被 BM 并入旧组
- RTL-SDR（librtlsdr）+ Android USB Host/OTG 采集

尚未实现（后续阶段）：
- BM→RF 发射（RF 侧仅接收）
- 用空口 Voice LC Header（全 LC + BPTC/RS）替代滞后的嵌入 LC，消除新发射头 ~0.3s 丢帧
- RF 色码显示（当前 DSDcc 报 0；不影响 BM 路由）

## RF 接收链接入方式

RF 侧只向上暴露 `DMRFrame`。你的 DMR PHY/解码器（或后续内置 4FSK 链）只需实现
`bmrf::rf::IDmrBurstSource`，`RfReceiver` 就会把突发解码并送入与 BM 完全相同的
CallManager / 语音管线 / 事件总线。也可用 `createFileBurstSource()` 回放突发抓包。

## 目录结构

```
BMRF-Gateway/
├── core/                      # 平台无关 C++17 核心库 bmrf_core
│   ├── include/bmrf/
│   │   ├── common/            # types / time / log / sha256
│   │   ├── dmr/               # frame / call / event / event_bus / call_manager
│   │   ├── brandmeister/      # homebrew / rx / tx / transport / client / network_state
│   │   ├── vocoder/           # ambe_decoder (IAmbeDecoder + Null + mbelib 预留)
│   │   └── audio/             # voice / jitter_buffer / audio_output / voice_pipeline
│   └── src/...
├── jni/                       # JNI 桥接（Android 通过 externalNativeBuild 编译）
│   └── bmrf_jni.cpp
├── android/                   # Android 应用（Gradle 模块 :app，目录名为 android）
│   └── src/main/{java,res,cpp,AndroidManifest.xml}
├── tools/                     # 命令行工具（frame_dump、RF 离线解码、BM 上行探针等）
│   ├── frame_dump.cpp
│   ├── bm_uplink_probe.cpp    # 诊断：IQ 回放 → 真实 HomebrewClient → BM（可抓 Last Heard）
│   └── rf_*.cpp
├── tests/                     # 单元测试（无第三方依赖）
├── CMakeLists.txt             # 桌面/CI 构建核心库、测试、工具
├── settings.gradle            # Android 构建入口
└── build.gradle
```

## AMBE 语音解码（mbelib）

真实语音解码使用 **vendored mbelib**（`third_party/mbelib/`，ISC 类许可，见其
`COPYRIGHT`）。Android 构建默认启用；桌面构建用 `-DBMRF_USE_MBELIB=ON`：

```bash
cmake -S . -B build-mbelib -DBMRF_USE_MBELIB=ON
cmake --build build-mbelib
```

解码路径（与 DroidStar / DSD-FME 一致，非脚手架）：

1. 33 字节 Homebrew DMR payload → 重排为 27 字节（3×9 字节 AMBE 帧），
   跳过交织在 14..19 的 6 字节嵌入信令，并合并第 14 字节的高/低半字节；
2. 每 9 字节（72 bit）AMBE 帧按 DMR 交织表 `rW/rX/rY/rZ` 填入 mbelib 的
   `ambe_fr[4][24]`；
3. 调用 `mbe_processAmbe3600x2450Frame()` 合成 160 个 PCM 采样；
   3 帧/突发 → 480 采样（60 ms @ 8 kHz）。

当 `BMRF_USE_MBELIB` 未开启时回退到 Null 解码器（静音），音频管线与 UI 状态仍正常。

> 设计文档 §16 提醒：AMBE 算法受专利约束，实际使用/分发请自行评估。

## 桌面构建与测试（核心库）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/tools/bmrf_frame_dump --help
```

## Android 构建

```bash
# 需要在项目根目录 local.properties 配置 sdk.dir（已加入 .gitignore）
# 需要 Android SDK + NDK (26.1) + CMake 3.22.1，JDK 17/21

# Debug
./gradlew :app:assembleDebug

# Release（带 release 签名，用于分发/发布）
./gradlew :app:assembleRelease
```

产物：

- `android/build/outputs/apk/debug/app-debug.apk`
- `android/build/outputs/apk/release/app-release.apk`（已签名）

**Release 签名**：密钥与口令放在 `android/keystore.properties`（**已加入 `.gitignore`，
切勿提交**）：

```properties
storeFile=keystore/bmrf-release.jks
storePassword=******
keyAlias=bmrf
keyPassword=******
```

生成密钥（首次）：

```bash
keytool -genkeypair -v -keystore android/keystore/bmrf-release.jks \
  -alias bmrf -keyalg RSA -keysize 2048 -validity 10000 \
  -dname "CN=BMRF-Gateway, O=BMRF, C=CN"
```

没有 `keystore.properties` 时 release 构建会**不签名**（仍可编译），因此不影响他人构建。
`build.gradle` 会自动读取该文件并应用到 `release` buildType。

本机已验证的组合：Gradle 8.7 + AGP 8.5.2 + JDK 21（Android Studio JBR）+
SDK `E:/Android_SDK` + NDK 26.1.10909125。

## 协议参考

- BrandMeister `go-brandmeister`（Homebrew 登录/配置/keepalive 的权威实现）
- MMDVMHost `DMRNetwork.cpp`（`DMRD` 报文 55 字节与 options 位布局）
- ETSI TS 102 361-1（DMR Air Interface）
- 设计文档：`../android_rtlsdr_dmr_brandmeister_design.md`

## License

This project is licensed under the **GNU General Public License v3.0** — see
[LICENSE](LICENSE).

It builds on and/or vendors third-party components under their own licenses:

- **MMDVMHost** (GPL-2.0-or-later) — reference for the DMRD / Homebrew protocol.
- **DSDcc / DSD-FME** (GPL-3.0) — DMR demodulation and decoding (`third_party/dsdcc`).
- **mbelib** (ISC-style) — AMBE 3600x2450 decoding (`third_party/mbelib`).
- **liquid-dsp** (MIT) — DSP primitives (`third_party/liquid-dsp`).
- **librtlsdr** (GPL-2.0) — RTL-SDR support.

> AMBE is patent-encumbered. Evaluate usage and distribution accordingly
> (see the design document §16).
