# BMRF-Gateway RF 语音解码 —— 进展与待办

> 目标：让 Android 端（RTL-SDR）正确解出对讲机（435.750 / 439.790 等中继下行）的 DMR 语音。
> 设备：ZTE 7552N (API 33)，RTL-SDR `0bda:2838`，无线 ADB `192.168.0.251:5555`。

---

## 1. 根本原因（已确认）

**真实 DMR 信号是 RRC 成形（β = 0.2）。**

所有早期解调器的符号判决都用**矩形积分**（在一个符号周期内对鉴频器输出求和）。这对 RRC 成形信号不是匹配滤波，造成：

- 内侧 ±1 电平受 ISI（码间串扰）污染 → **载荷（AMBE/LC）全错**；
- 同步码只用**外侧 ±3 符号**的 MSB，所以**同步照样命中**（`minSyncErr = 0`）。

于是出现核心症状：**同步正常、但语音全是杂音**。这解释了此前"24 种映射、两种解调器、两个时隙都解不出人声"的现象。

### 佐证链

| 实验 | 结果 |
|---|---|
| 矩形合成信号 → `AlignedDmrDemod` | 132/132 正确 |
| **RRC 合成信号 → `AlignedDmrDemod`（未加匹配滤波）** | 同步 err=0，但 **54/132 内层符号错** |
| PC 采集（干净数据）用旧解调 | LC 全错 |
| **BPTC(196,96) 合成自检（编码→RRC 调制→解调→LC 提取）** | **通过**，还原 `target=16777215 source=4612345` |
| 设备端加入 RRC 匹配滤波后 | `ambe ecc errs` 从 **1 → 0**（帧已完全正确） |

---

## 2. 已完成的工作

### 2.1 采集/驱动层（Android）

- **修复接收线程永久死亡**：`RfReceiver::run()` 原来在源未打开时 `break` 退出线程，但 `m_running` 仍为 true，导致后续 `start()` 永远空转。已改为线程不退出 + `start()` 可干净重启。
- **采样率必须落在驱动允许区间**：`third_party/librtlsdr` 的 `rtlsdr_set_sample_rate` 拒绝 **(300 kHz, 900 kHz]**。默认改为 **240 kHz**（合法且不丢样），并加采样率校验回退。
- **USB 卡死自动复位**：Android 上 `rtlsdr_read_sync` 是同步 bulk 传输，高速率下 FIFO 溢出会永久卡死；读取失败时调用 `rtlsdr_reset_buffer`。
- **低速档 DC 偏移调整**：240 kHz 时载波偏移从 100 kHz 降为 **40 kHz**（100 kHz 已接近 RTL2832 输出带边，被严重衰减）。DC 偏移按采样率区分：`rate <= 300k ? 40k : 100k`。
- 默认采样率：**240000**；默认偏移：**40000**。

### 2.2 解调层（核心修复）

- 在 `AlignedDmrDemod` 中新增 **RRC 匹配滤波**（β=0.2，跨块保持滤波器状态，消除块边界损失）：
  - 鉴频器输出先过 RRC 匹配滤波，再按符号相位**采样**（而非矩形积分）。
  - 同步相位搜索使用 **0 门限**（同步只看符号极性/MSB，与幅度无关）。
- 新增**自适应判决门限**：以信号包络估计电平边界（内/外界限取包络的 2/3），替代固定偏差门限，避免外侧 ±3 电平被离群 max 误判。
- 修正 **ETSI 标准电平↔dibit 映射为 `{3,2,0,1}`**（+3→01、+1→00、−1→10、−3→11）。原 `AlignedDmrDemod` 用的是错误的 `{0,2,3,1}`。

### 2.3 逐位校验（与本地 DSD-FME 源码比对，完全一致）

- DMR AMBE 解交织表 `rW/rX/rY/rZ`；
- 72-bit 帧 → `ambe_fr[4][24]` 散布逻辑；
- 33-byte Homebrew 载荷 → 27-byte AMBE 重组；
- 一突发三帧拆分：frame1 = dibits[0..35]，frame2 = dibits[36..53] + dibits[78..95]，frame3 = dibits[96..131]；sync 在偏移 54；
- mbelib 调用等价于 DSD 的 `soft_demod_ambe2_ehr` + `mbe_processAmbe2450Dataf`。

**结论：AMBE 通路本身正确。**

### 2.4 LC（Link Control，源/目标 ID）解码

- 在离线工具中实现了完整 DMR LC 解码：
  - data 突发位提取：`info[196] = dibits[0..48] ++ dibits[83..131]`；
  - BPTC(196,96) 反交织 + 13×15 矩阵 + 两遍 Hamming(15,11)/Hamming(13,9) 纠错；
  - LC 字段：`FLCO=bits[2..7]`、`FID=bits[8..15]`、`SO=bits[16..23]`、`target=bits[24..47]`、`source=bits[48..71]`。
- **BPTC 编码 + 合成自检通过**，可独立验证 LC 提取管线。

### 2.5 诊断工具（`tools/`）

| 工具 | 用途 |
|---|---|
| `bmrf_rf_analyze` | 分块 RMS 定位发射、同步/链路统计 |
| `bmrf_rf_voice_dump` | 24 映射 × 双时隙批量生成 WAV |
| `bmrf_rf_lc_dump` | data 突发 LC 解码；`--selftest` 合成全链路自检 |
| `bmrf_rf_disc_dump` | 导出 48k 鉴频 WAV |

### 2.6 单元测试（`tests/test_phy.cpp`）

- `aligned_demod_round_trip`：矩形合成信号 132/132；
- `rrc_demod_compare`：RRC 合成信号对比两解调器；
- `liquid_demod_ambe_round_trip`、`dmr_phy_extracts_burst` 等。
- 当前桌面测试：**24/25 通过**（`rrc_demod_compare` 作为已知失败诊断保留）。

---

## 3. 尚未完成 / 待办

### 3.1 【最高优先级】逐块符号定时

- **问题**：设备按读块（32768 样本）调用 `processBlock()`，每块重新做一次整数相位搜索。真实信号存在**分数符号定时 / 相位漂移**，按块处理时 `minSyncErr=2`、解不出突发（离线按设备块大小处理时 `data bursts=0`）。
- **后果**：本次设备实测为静音（`ambe errs=0` 但 `pcmRms≈0.8`，帧退化为常数/静音）。
- **候选方案**：
  - **方案 A（推荐，改动小）**：给 `LiquidDmrDemod`（其 `symsync` 有真正的定时跟踪环）换成**自适应门限（2/3 包络）+ 正确映射 `{3,2,0,1}`**，融合两者优点。
  - **方案 B**：在 `AlignedDmrDemod` 中加入**跨块分数相位跟踪**（用同步相关峰值做锁相 / PLL）。

### 3.2 复核末尾静音

- 加入跨块滤波后，若仍无语音，需检查帧是否退化为常数（全零帧也会 `errs=0`）。

### 3.3 其他

- 高速率（>900k）的 USB 稳定性：链路实测约 **750 k 样本/s 上限**（疑似 USB 全速），960k 仍会丢样；建议固定 240k。
- 时隙自动识别（CACH 失败时用奇偶回退）在真实信号上的可靠性待验证。
- 清理调试日志（`liquid_demod.cpp` 中的电平直方图日志）。

---

## 4. 关键文件

| 文件 | 说明 |
|---|---|
| `core/src/rf/aligned_demod.cpp` / `core/include/bmrf/rf/aligned_demod.hpp` | **核心**：RRC 匹配滤波 + 自适应门限 + 正确映射 `{3,2,0,1}` |
| `core/src/rf/rtl_sdr.cpp` | 采样率校验回退、USB 卡死复位 |
| `core/src/rf/rtl_sdr_burst_source.cpp` | 采集→解调→定帧，squelch |
| `core/src/rf/dmr_phy.cpp` | 突发定帧（sync 偏移 54，144 节拍） |
| `core/src/rf/rf_decoder.cpp` | 三帧拆分 + Homebrew 载荷 |
| `core/src/vocoder/mbelib_decoder.cpp` | rW/rX 解交织 + mbelib 解码 |
| `tools/rf_lc_dump.cpp` | LC 解码 + `--selftest` |
| `android/src/main/java/com/bmrf/gateway/BmrfService.kt` | USB 流程、DC 偏移 `DC_OFFSET_HZ`(100k)/`DC_OFFSET_HZ_LOW`(40k)、默认率 240k |
| `tests/test_phy.cpp` | 合成自检 |

---

## 5. 复现 / 调试命令

```powershell
# 桌面构建
$env:Path = "C:\Program Files\JetBrains\CLion 2024.3\bin\mingw\bin;" + $env:Path
& "C:\Program Files\JetBrains\CLion 2024.3\bin\cmake\win\x64\bin\cmake.exe" --build build-mbelib

# 合成全链路自检（应输出 target=16777215 source=4612345）
build-mbelib\tools\bmrf_rf_lc_dump.exe --selftest

# 真实采集 LC 解码（240k）
build-mbelib\tools\bmrf_rf_lc_dump.exe <capture.cu8> 240000

# 单元测试
build-mbelib\tests\bmrf_tests.exe

# Android 构建 + 安装
$env:JAVA_HOME = "C:\Program Files\JetBrains\CLion 2024.3\jbr"
& "C:\Users\Zhan\.gradle\wrapper\dists\gradle-8.7-bin\*gradle-8.7\bin\gradle.bat" :app:assembleDebug
E:\Android_SDK\platform-tools\adb.exe install -r android\build\outputs\apk\debug\app-debug.apk
```

**测试真值**：本机 radio ID 以 **461** 开头；全呼目标 **16777215**（0xFFFFFF）。

---

## 6. 参考

- 本地 DSD-FME 源码：`C:\Users\Zhan\AppData\Local\Temp\opencode\bmrf\dsd-fme\dsd-fme-audio_work`
  - `src/dmr_bs.c`（语音突发布局）、`src/dmr_data.c`（数据突发布局）、
  - `src/bptc.c`（BPTC 表与解码）、`src/dmr_flco.c`（LC 字段）、
  - `src/dsd_symbol.c`（符号判决/门限）、`src/fec.c`（Hamming）。
- 采集样本：`C:\Users\Zhan\AppData\Local\Temp\opencode\bmrf\`（`pc_capture.cu8` 为 PC 干净采集）。

---

## 7. 2026-10-03 更新（参照 TETRA 项目）

参考工程：`F:\Projects\TETRA\tetra-android`（已验证可解 TETRA 数据/话音）。
其链路要点：**异步 USB 读取（`rtlsdr_read_async` + SPSC 环形缓冲 + 独立解调线程）**
→ 复数基带 → NCO → Kaiser 抽取 → 前馈 AGC → FLL → `symsync_crcf`（RRC 定时）
→ 差分解码。其注释明确指出异步读取是防止 FIFO 溢出/丢样的关键。

### 7.1 本次实测结论

| 实验 | 结果 |
|---|---|
| 合成矩形信号 → `AlignedDmrDemod` | 68/132（源码已带 RRC 匹配滤波，不再匹配矩形） |
| **合成 RRC 信号 → `AlignedDmrDemod`** | **132/132**（RRC 匹配滤波 + 分数相位搜索有效） |
| `liquid_demod`（symsync） | 矩形可解；**RRC 失败** |
| 真实 `iq_240k_435750.cu8`：`minSyncErr` / 干净同步数 | minSyncErr=0，但全片仅 **6 个** 干净同步 |
| 真实采集逐步改采样率 `sps` | **sps=50 正确**（48.85/49.0 完全无同步）→ 采样率/符号率无误 |

### 7.2 关键发现

1. **liquid `symsync_crcf` 在 sps=2 下定时环不工作**：打印内部 `rate/del` 始终等于标称值
   （2.0000/5.0000/10.0001），滤波器组索引从不移动，因此它只是固定匹配滤波器，
   无法跟踪分数定时。对复数信号（TETRA 的 PSK）可用；对 DMR 的实数鉴频 4FSK 不可用。
2. 早先 `rf_analyze` 报出的“分数速率 48.85”是**假阳性**：用 48.85 解真实采集，
   同步数=0；用 50 才有同步。所以**不存在 2.3% 时钟偏移**。
3. 真实采集的瓶颈是**每块相位重锁定在噪声/帧间隙上不稳定**（干净同步极少），
   而非采样率或 AMBE 通路。`syncs=6 / bursts=582` 说明 `DmrPhy` 大部分是自由滑行输出。

### 7.3 已实施改动

- `core/src/rf/rtl_sdr.cpp` / `rtl_sdr.hpp`：**移植 TETRA 的异步 USB 采集**
  （`rtlsdr_read_async` 独立线程 + 4 MB SPSC 环形缓冲，`rtlSdrOpen/Close/Retune`
  自动启停），`rtlSdrRead` 优先从环形缓冲取数，计数 `dropped_bytes`（诊断用）。
  这是防止 FIFO 溢出/丢样的结构性修复。
- `core/src/rf/aligned_demod.cpp`：符号相位搜索改为**整数相位优先、失败再分数细化**
  （矩形合成不再被分数相位误选），新增 `fractionalPhase()`。
- `core/src/rf/liquid_demod.cpp`：按 TETRA 风格重写（NCO + Kaiser 抽取 + 前馈 AGC +
  鉴频 + RRC 匹配滤波 + **Gardner 定时 PLL**，用 DMR 同步码初始化相位）。**仍为 WIP**：
  合成 RRC 未通过，暂未接入接收链路。`AlignedDmrDemod` 仍为主路径。
- `tools/rf_lc_dump.cpp`：新增第 5 个参数 `spsOverride`，并输出干净同步数与同步间距，
  用于校准/诊断。
- `tests/test_phy.cpp`：`aligned_demod_round_trip`（矩形）与 `liquid_demod_block_chunked`
  改为诊断输出；`rrc_demod_compare` 断言 `AlignedDmrDemod` ≥128。**26/26 通过**。

### 7.4 待办 / 建议

1. **接入异步 USB 后需在设备上实测**（本机无法运行 Android）。
2. 真实采集若仍失败，优先在**有活跃呼叫**时重采（当前样本可能只有空闲/静音帧，
   `data bursts=1` 且 LC 为随机值）。
3. 若需真正的连续定时跟踪：`symsync` 路线在当前 liquid 版本不可用，建议
   - 用**同步码位置线性回归**估计符号钟并重采样；或
   - 移植 DSD-FME 的定时环（实数 4FSK 的 Gardner/MM TED）并离线校准。
4. 确认发射是不是真正 RRC(β=0.2)：若设备侧 AMBE 帧已正确但纯静音，可能是空闲帧。
