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

## 当前实现范围

对应设计文档 §32 的 ①②③④，以及 BM 下行语音链路（MVP-2）：

- 统一数据模型：`DMRFrame` / `DMRCall` / `DMREvent`
- 线程安全事件总线 `EventBus`
- 呼叫状态机 `CallManager`（呼叫识别、开始/结束、帧计数、丢帧统计、超时、会话统计）
- BrandMeister Homebrew **RX** 解析（`DMRD` 55 字节、`MSTACK/NAK/PING/CL`、`RPTPONG`）
- BrandMeister Homebrew **编码**（`RPTL` / `RPTK` / `RPTC` / `DMRD` / `MSTPING`），
  含 `SHA-256(nonce || password)` 登录校验
- 网络状态机 `HomebrewClient`（DISCONNECTED→CONNECTING→REGISTERED→ACTIVE→RECONNECTING）
- **下行语音管线**：`DMRFrame` → `JitterBuffer` → `IAmbeDecoder` → `IAudioOutput`，
  独立工作线程，Android 端使用 **AAudio**（8 kHz mono）
- **可插拔 AMBE 解码器**：默认 Null 解码器（静音），预留 `BMRF_USE_MBELIB` 开关
- JNI 桥接层（返回 JSON 状态 + 事件快照）
- **仪表盘 UI**：当前呼叫卡片、网络状态、会话统计（呼叫数/帧数/丢帧/丢帧率）、
  最近呼叫列表、音频状态与播放开关；前台服务承载核心
- 单元测试（Homebrew 解析、Call Manager、JitterBuffer/语音管线）与 `bmrf_frame_dump`

**RF 上行接收（协议层基础，stage RF-1）**
- `rf/dmr_sync`：DMR 48-bit 同步码分类（BS/MS × 语音/数据）
- `rf/fec`：clean-room Hamming(7,4)（CACH TACT）+ CRC-CCITT / 5-bit
- `rf/burst`：`DMRBurst`（132 dibit / 264 bit 突发模型 + 度量）
- `rf/rf_decoder`：突发 → `DMRFrame(source=Rf)` 语音路径（3×72bit AMBE →
  Homebrew 兼容 33B payload，复用 mbelib 解码）
- `rf/sample_source`：`ISampleSource`（cu8 IQ）+ Null 源
- `rf/burst_source`：`IDmrBurstSource` + 文件回放源（设计文档 §21）
- `rf/rf_receiver`：独立线程，突发源 → 解码 → CallManager / VoicePipeline / EventBus

尚未实现（后续阶段）：
- RF 的 CACH 色码（Golay 20,8）与 BPTC/LC 解码（RF 呼叫的 Source/Destination ID）
- 4FSK 解调 + 符号定时恢复 + 同步捕获
- RTL-SDR（librtlsdr）+ Android USB Host/OTG 采集
- RF→BM 上行编码、BM→RF 发射

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
├── tools/                     # 命令行工具
│   └── frame_dump.cpp
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
./gradlew :app:assembleDebug
```

产物：`android/build/outputs/apk/debug/app-debug.apk`（含 arm64-v8a / armeabi-v7a / x86_64）。

本机已验证的组合：Gradle 8.7 + AGP 8.5.2 + JDK 21（Android Studio JBR）+
SDK `E:/Android_SDK` + NDK 26.1.10909125。

## 协议参考

- BrandMeister `go-brandmeister`（Homebrew 登录/配置/keepalive 的权威实现）
- MMDVMHost `DMRNetwork.cpp`（`DMRD` 报文 55 字节与 options 位布局）
- ETSI TS 102 361-1（DMR Air Interface）
- 设计文档：`../android_rtlsdr_dmr_brandmeister_design.md`

## 许可证

核心协议实现参考了 GPL-2.0（MMDVMHost）与 BrandMeister 官方仓库，实际分发前
请自行确认各上游的许可与专利状态（尤其是 AMBE 编解码，见设计文档 §16）。
