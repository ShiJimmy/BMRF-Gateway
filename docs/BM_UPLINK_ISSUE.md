# BMRF-Gateway — RF → BrandMeister 上行无法送达 问题交接文档

> 目的：把"RF 发射能解出来、但 BrandMeister 上查不到我们的呼叫"这个**未解决问题**、已做过的所有尝试、
> 关键证据、剩余假设、以及复现/构建方法，完整记录，方便在**新窗口**继续。

---

## 0. 一句话现状

- App **能登录 BM**（顶部 ACTIVE / LOGIN yes），**能收 BM 下行**、能本地播放、能解 RF 语音。
- **RF 发射经过 App 转发到 BM 后，BM 的 Last Heard 里查不到我们的呼叫。**
- 用 **DroidStar**（同一 DMR ID + 同一热点密码）在 BM 上**能出现记录** → **账号/热点本身没问题**，
  问题在我们 App 的这一段上行。

---

## 1. 环境 / 路径 / 常用命令

- 工程根：`F:\Projects\MMDVM\BMRF-Gateway`
- 设计文档：`F:\Projects\MMDVM\android_rtlsdr_dmr_brandmeister_design.md`
- 备份（可用可回退）：`F:\Projects\MMDVM\backup-20261004-2305`（含源码 + 当时 APK `app-debug-current.apk`）
- 设备：ZTE 7552N (Android, API 33)，无线 adb `192.168.0.251:5555`
  - 注意：还存在另一个 adb 设备 `323950788646`，**始终用 `adb -s 192.168.0.251:5555`**。
  - 无线 adb 会掉线：`adb connect 192.168.0.251:5555` 重连。
- RTL-SDR：`0bda:2838`，实际发射 CC12 / TS1（但 App 解出 CC=0, TS=1/-，见 §7）。
- 测试事实：本机电台 DMR ID **4615543**（BG7KNO），热点/中继 ID 也是 **4615543**。
  RF 上看到 `src=4615543 dst=46001/46073` 等（本地中继）。

### 构建 / 部署

- 桌面（跑测试 / 工具）：
  - 配置已存在 `build-mbelib`，参数为 `-DBMRF_USE_MBELIB=ON -DBMRF_USE_DSDCC=ON`。
  - 构建：`cmake.exe --build build-mbelib`（cmake 在 `C:\Program Files\JetBrains\CLion 2024.3\bin\cmake\win\x64\bin`）
  - mingw 需在 PATH：`C:\Program Files\JetBrains\CLion 2024.3\bin\mingw\bin`
  - 测试：`build-mbelib\tests\bmrf_tests.exe`（当前 **32/32 通过**）
- Android：
  - `JAVA_HOME=C:\Program Files\JetBrains\CLion 2024.3\jbr`
  - gradle：`C:\Users\Zhan\.gradle\wrapper\dists\gradle-8.7-bin\bhs2wmbdwecv87pi65oeuq5iu\gradle-8.7\bin\gradle.bat`
  - **注意：gradle 经常误判原生无需重编**。可靠做法：**先 `gradle clean` 再 `:app:assembleDebug`**（否则 APK 里可能是旧 `.so`）。
  - 安装：`adb -s 192.168.0.251:5555 install -r android\build\outputs\apk\debug\app-debug.apk`
- 抓日志：`adb -s 192.168.0.251:5555 logcat -d | Select-String "BMRF/"`

---

## 2. 上线路径（核心代码）

```
RTL-SDR IQ → DSDcc 前端(NCO去偏/降采样48k/FM鉴频) → DSDcc DMR 解码
   → dsdcc_demod.cpp: 取出 27 字节 AMBE(每burst) → buildHomebrewPayload → 33字节 payload
   → rf_receiver.cpp: 组 DMRFrame(源/目标/slot/CC/FLCO/BER) → 只转发LC有效(地址FEC OK)的帧
   → client.cpp sendFrame(): 合成 Voice LC Header(每次呼叫一次) + 语音帧 + Terminator
        · 语音帧叠加 DMR 嵌入LC/EMB（applyVoiceSignalling）   ← 本次新增
        · 组 DMRD → writeRaw → UDP → BrandMeister
```

关键文件：
- `core/src/rf/dsdcc_demod.cpp` / `core/include/bmrf/rf/dsdcc_demod.hpp` — DSDcc 包装、squelch、AMBE/FEC 计数、地址/FLCO/嵌入LC getter。
- `core/src/rf/rf_receiver.cpp` — RF→统一模型桥接、LC有效门控、转发。
- `core/src/brandmeister/client.cpp` — 登录状态机、sendFrame/sendLcHeader/sendTerminator、LC 投票、嵌入信令叠加。
- `core/src/brandmeister/uplink.cpp` — DMRFrame→DMRData（options 字节布局）。
- `core/src/brandmeister/tx.cpp` — buildLogin/buildKey/buildConfig(RPTC)/buildDmrData/buildPing/buildClose。
- `core/src/dmr/lc_encode.cpp` (+ `lc_tables.inc`) — LC 头/结束帧编码(BPTC/RS/Golay/同步图案) + 语音嵌入LC(本次新增)。
- `jni/bmrf_jni.cpp` — nativeConfigure（Host/ID/密码/软件ID/包名/频点/CC）。
- `android/.../BmrfService.kt` / `MainActivity.kt` — UI/服务/设置。

---

## 3. 协议要点（已确证）

### 3.1 登录握手（经典 ASCII，已工作）
- `RPTL` + **8 位十六进制 ID** → master 回 `MSTACK` + 8hex id + 8hex salt。
- `RPTK` + 8hex id + `sha256(salt(8 ASCII) + password)` 的**十六进制** → `MSTACK`。
- `RPTC`(配置) → `MSTACK`。
- `MSTPING` → `RPTPONG`（keepalive）。
- **本 App 用的就是这套**，能到 ACTIVE。

### 3.2 下行 DMRD options = **go-brandmeister 布局**（已确证，修复了丢包）
- `bit0` slot、`bit1` private、`bit2-3` frameType(0 voice,1 voiceSync,2 data)、`bit4-7`=N/数据类型。
- 之前按 MMDVMHost 旧布局解析 → 帧号乱跳(假丢包) + 群/私翻转 → 换成 go-brandmeister 后丢包=0。
- 代码：`core/src/brandmeister/homebrew.cpp`（getter）、`rx.cpp`。

### 3.3 上行 DMRD options = **MMDVMHost 布局**（与 DroidStar 一致）
- `0x80` slot2、`0x40` private、`0x20` dataSync、`0x10` voiceSync、低四位 = N/数据类型。
- 代码：`core/src/brandmeister/uplink.cpp`。
- **当前为了实验被强制为 TS2**（`options |= 0x80` 无条件）——见 §6 待办。

### 3.4 data-sync 突发必须带 **DMR 同步图案**（本次修复的关键之一）
- VoiceHeader/Terminator 的 33 字节 payload 的 **13..19 字节**要写入同步图案。
- MS-sourced data sync = `0D 5D 7F 77 FD 75 70`，掩码 `SYNC_MASK={0F,FF,FF,FF,FF,FF,F0}`。
- 代码：`core/src/dmr/lc_encode.cpp::encodeDmrLcPayload`。
- **证据**：master 下行头 payload 14..18 = `5D 7F 77 FD 75` = 该图案；加之前我们写 0 → BM 可能丢弃。

### 3.5 语音帧必须带 **DMR 嵌入LC + EMB**（本次修复的关键之二）
- 每语音帧 payload 字节 13..19 要放：
  - N=0：改为 Voice Sync（options|=0x10），写 **音频同步图案** `07 F7 D5 DD 57 DF D0`。
  - N=1..4：放嵌入LC分片（32bit/帧，CRC5 + Hamming(16,11,4) + 列交织，从 128bit 块切）。
  - N=5：该区间清零。
  - 另叠加 EMB（字节13,14,18,19）：`(cc<<4)|(lcss<<1)` 经 QR(16,7) 编码。
- 代码：`core/src/dmr/lc_encode.cpp::applyVoiceSignalling`，由 `client.cpp sendFrame` 调用。
- **已用 DroidStar 抓到的真实语音帧逐字节验证通过**（N=1..4 的字节15..17 = `91e142 / b1e053 / 7170f2 / f18283`）。

---

## 4. 决定性证据：DroidStar(能用) vs 我们的 App(不能用)

DroidStar 开 `Settings → "Debug output to stderr"` 后，`logcat` 里会打印 `SEND:` 原始 DMRD。

### 4.1 DroidStar 的 VoiceHeader（DMRD）
```
44 4d 52 44 | 00 | 466d77 | 466d77 | 00466d77 | a1 | 7c0c446d |
0f0a07f4774c7b4062a055a2c06d5d7f77fd7573ae4c0e50c6217ae161057a0676 | 0000
```
- `options=0xa1` = TS2(0x80)+dataSync(0x20)+VoiceHeader(1)
- payload 13..19 = `6d5d7f77fd7573`（含 data sync）

### 4.2 我们 App 的 VoiceHeader（DMRD，同一 LC：src=dst=4615543）
```
... 00466d77 | a1 | <stream> |
0f0a07f4774c7b4062a055a2c06d5d7f77fd7573ae4c0e50c6217ae161057a0676 | 0000
```
- **33 字节 payload 与 DroidStar 逐字节完全相同**！options 也相同(a1)。
- 结论：**头的编码没有差别**。

### 4.3 DroidStar 的语音序列（节选，TS2）
```
seq=0x54 opt=0x85  (voice N=5)  payload ... 10 00 00 00 00 00 04 ... (嵌入LC)
seq=0x55 opt=0x90  (voice sync) payload ... 07 f7 d5 dd 57 df d3 ... (音频同步)
seq=0x56 opt=0x81  (voice N=1)  payload 字节15..17 = 91 e1 42
...
seq=0x61 opt=0xa2  (Terminator)
```
我们 App 现在也发 header + 嵌入LC语音帧 + voice-sync + Terminator（§3.4/3.5），
并已逐字节验证嵌入LC。**但仍然不进 BM。**

---

## 5. 已排除 / 已尝试（含结果）

| # | 尝试 | 结果 |
|---|------|------|
| 1 | 自定义 demod 无法解 4FSK → 换 **DSDcc** 解码 | ✅ RF 能解出语音(src/dst/CC/AMBE) |
| 2 | 下行 options 布局 → 改 go-brandmeister | ✅ 丢包 70%→0，群/私不再翻转 |
| 3 | 上行 options 布局 MMDVMHost ↔ go-brandmeister 反复 | ❌ 两种都进不了 BM |
| 4 | RF→BM 只发语音帧（无头） | ❌ 不进 BM |
| 5 | 合成 Voice LC Header（单纯 BPTC，**无同步图案**） | ❌ |
| 6 | Header + Terminator | ❌ |
| 7 | LC 头重复发(每1.5s)+前3帧都发 | ❌（且怀疑重复头反而不利，已改回一次） |
| 8 | 10 帧 LC 投票后才开呼（保证干净 LC）| ❌（LC 有效率≈82%） |
| 9 | **补 data-sync 同步图案（§3.4）** | ❌ 仍不进（但确为必要，已与 master 对照验证） |
| 10 | 强制上行 **TS2**（与 DroidStar 一致） | ❌ |
| 11 | **补语音嵌入LC+EMB+voice-sync（§3.5）** | ❌（逐字节已验证，仍不进） |
| 12 | RPTC 配置改为 **MMDVMHost 格式**(302B，4字节原始ID) | ❌（登录仍 ACTIVE） |
| 13 | 上报软件/包名改为 **MMDVM_HS / MMDVM_MMDVM_HS_Hat** | ❌（最近一次，结果"仍然失败"） |

其它已确认正常的部分（与上行无关，勿动）：
- 登录 ACTIVE、下行接收与播放、RF 本地播放、RSSI/SNR/BER 显示、FLCO/SvcOpt/ColorCode/时隙显示、
  BM 通话按组筛选显示、下行播放开关、RF 上行时静音下行(ducking)、连接字段与开关持久化、
  点空白收键盘、组订阅 API、历史 50 条、分层抓包(capture) 等。

---

## 6. 剩余假设（新窗口从这里继续）

按可能性排序：

1. **DMRD 根本没送到 master（发送路径问题）**
   - App 日志只能在 `writeRaw` 里看"构建出的字节"，看不到发送结果（55 字节包不打印，失败才有 warn）。
   - **下一步**：给 `UdpTransport::send` / `writeRaw` 加"每 N 帧打印发送结果/目标地址/长度"的日志，
     或加计数器暴露到状态里，确认 DMRD 真发出、发到正确地址端口。
   - 也可对比：MSTPING(15B) 能收到 RPTPONG，DMRD(55B) 是否真的 send() 成功且对端是 master。

2. **master 侧会话状态 / 发射授权**
   - 同一 ID，DroidStar 能发、我们不行 → 可能是**会话属性**（RPTC 解析后 master 认为本设备类型不同）。
   - **下一步**：把 RPTC 配置**完全照抄 DroidStar**（软件ID=`20200922`、包名=`MMDVM_MMDVM_HS_Hat`、
     power=1、cc=1、height=0、slots=`4`、lat/lon=0），逐字节对齐后再试。
   - 检查 BM **Device Logs**（SelfCare→Device Logs）在发射时是否出现 `wrong configuration` / `not permitted`。

3. **master / 主机不同**
   - 我们连 `4602.master.brandmeister.network`；DroidStar 的 host 名是 `BM_4602_China`（可能指向**另一台** master）。
   - **下一步**：对比两者实际解析到的 IP；可临时把 App 的 host 改成 DroidStar 用的那台 master 再试。

4. **发送时序 / 速率**
   - DroidStar 头后紧接每 ~57ms 一帧语音；我们受 RF 到达节奏+10帧投票影响。
   - **下一步**：打印每帧发送时间戳，确认节奏；必要时去掉投票（DroidStar 不投票）。

5. **DMRD 里某字段被 master 校验**
   - 目前头与 DroidStar 逐字节相同，但仍需确认 **流ID/序号/repeater字段** 完全：
     `[4]=seq, [11..14]=0x00466d77, [16..19]=stream, [53][54]=ber/rssi(我们也许非0)`。
   - **下一步**：把 `toDmrData` 的 `data.ber/rssi` 也置 0（DroidStar 是 0），保持与 DroidStar 一致。

6. **发送到 master 的源端口/绑定**
   - transport 用单个 socket；确认 DMRD 与登录同一 socket 发出。

---

## 7. 现场数据（供新窗口核对）

- App 运行日志关键字（`BMRF/` 前缀）：
  - `BM (…): state … -> ACTIVE` 登录成功
  - `BM/tx: Voice LC header src=… dst=… <hex>` 合成的头
  - `BM/tx: UPLINK <hex>` 语音帧
  - `BM/tx: Voice terminator …` 结束帧
  - `BM/rx: DMRD …` 下行数据
  - `rf: LC valid=x/y (%)` RF 侧 LC 有效率（约 82%）
  - `BM/rx: <hex>` 非 55 字节收包（MSTACK/MSTNAK/RPTPONG…）
- UI 显示：FLCO=0x00、Svc Opt=0x00、Color Code=0（实际发射 CC=12）、Timeslot=1/-（实际 TS1）。
  → 这些是**解码显示**信息，**大概率不影响 BM 收帧**（BM 按组路由，不校验 CC/时隙），
    但 CC=0 说明我们解出的色码不对（DSDcc getColorCode 值得复查，见 `dsdcc_demod.cpp colorCode()`）。

---

## 8. 抓包 / 工具

- `tools/rf_dsd_dump.cpp`、`rf_lc_dump.cpp`、`rf_voice_dump.cpp`、`rf_analyze.cpp`、`rf_disc_dump.cpp`：
  离线解 IQ（`C:\Users\Zhan\AppData\Local\Temp\opencode\bmrf\talk1920.cu8` 等）。
- 分层抓包：`bmrf::capture`（`rf_frame`/`bm_tx`/`bm_rx`），App 开 "Record IQ" 时写 `bmrf_capture.txt`
  （`getExternalFilesDir`），配合 `tools/capture_replay.cpp` 回放。
- DroidStar 源码参考：`github.com/nostar/DroidStar`（`dmr.cpp`、`crs129.cpp`、`cbptc19696.cpp`、
  `cgolay2087.cpp`、`CRCenc.cpp`、`DMRDefines.h`）——已确认我们头/嵌入LC与其一致。

---

## 9. 当前代码状态提醒（重要）

- `core/src/brandmeister/uplink.cpp`：**options 被强制 TS2**（临时实验，`options |= 0x80;` 无条件）。
  需决定：要么恢复按 `frame.timeslot`，要么做成可配置。
- `jni/bmrf_jni.cpp`：软件/包名被改成 `MMDVM_HS` / `MMDVM_MMDVM_HS_Hat`（实验）。
- `core/src/brandmeister/tx.cpp buildConfig`：已改为 MMDVMHost(302B) 格式（实验）。
- `core/src/dmr/lc_encode.cpp`：已加入**同步图案 + applyVoiceSignalling（嵌入LC）**（这两项已被 DroidStar 字节验证，建议保留）。
- 测试：`tests/test_lc_encode.cpp` 有 DroidStar 字节比对；`test_uplink.cpp` 因强制 TS2 断言 bit7=1。
- 无 git；改动都在工作区。备份在 §1。

---

## 10. 建议的新窗口第一步

1. **确认 DMRD 真的发出**（假设 1）：在 `writeRaw`/`transport->send` 加日志/计数，实机发射时确认
   55 字节包成功 send 到 master。若没发出 → 修发送路径；若发出 → 转假设 2/3。
2. **完全照抄 DroidStar 的 RPTC**（假设 2）再试，并看 BM Device Logs。
3. **对比 master IP**（假设 3）：临时改用 DroidStar 的 master。
4. 若都无效，考虑**直接用 DroidStar 那台可用的客户端 + 我们的 RF**作为过渡方案，
   或把"上行发送"改为复用 DroidStar 已验证的完整 DMRD 构造流程（逐字节对齐其 `send_frame`）。
