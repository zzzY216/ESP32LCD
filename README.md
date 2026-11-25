# ESP32 BLE → STM32 图像桥

手机 App 通过 BLE 下发数据，ESP32 打包成协议帧经 UART 转发给 STM32，
STM32 驱动 ST7789 屏幕显示。

```
手机 App ──BLE──► ESP32-S3 ──UART2(921600)──► STM32F103 ──SPI──► ST7789 240x240
```

> ⚠️ **确认芯片型号：本项目运行在 ESP32-S3 上**（不是经典 ESP32）。
> 从启动日志可确认：
> ```
> ESP-ROM:esp32s3-20210327
> boot.esp32s3: Boot SPI Speed : 80MHz
> cpu_start: GPIO 44 and 43 are used as console UART I/O pins
> ```
> 这意味着：
> - 控制台 UART0 在 **GPIO43/44**（不是 ESP32 的 GPIO1/3）
> - 没有 `CONFIG_BTDM_CTRL_MODE_*` 这些 ESP32 专有配置项
> - 引脚编号可用到 GPIO48

---

## 一、快速开始

### 1. 构建

```powershell
cd D:\esp32\MyProject\OLCD
.\build.ps1
```

### 2. 烧录

```powershell
.\build.ps1 -Flash
# 指定端口
.\build.ps1 -Flash -Port COM3
```

### 3. 烧录并看日志

```powershell
.\build.ps1 -Monitor
```

### 为什么需要 `build.ps1` 而不是直接 `idf.py build`

你的 ESP-IDF 是用 **EIM** 装的，它的激活脚本有两个问题：

1. **没设置 `ESP_IDF_VERSION`** → `idf.py` 启动时直接报
   `TypeError: expected string or bytes-like object, got 'NoneType'`
2. **没把 cmake / ninja / 工具链加进 PATH** → 报
   `"cmake" must be available on the PATH`

`build.ps1` 把这两件事都补齐了。如果你想手动敲命令，先执行：

```powershell
$env:IDF_PATH='C:\esp\v6.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'
$env:ESP_IDF_VERSION='6.1.0'
$env:PYTHONIOENCODING='utf-8'
$env:PYTHONUTF8='1'
$env:PATH="C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;" +
          "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;" + $env:PATH
```

---

## 二、接线

```
ESP32                     STM32F103C8
─────────────────────────────────────────
GPIO17 (TX2) ────────►   PA10 (RX)
GPIO16 (RX2) ◄────────   PA9  (TX)
GND          ─────────   GND        ← 必须共地
```

**TX 接对方的 RX**（交叉连接）。

详细说明见 STM32 项目的 `docs/WIRING.md`。

---

## 三、BLE 服务

### 设备名

```
OLCD-ESP32
```

### GATT 结构

| 项目 | UUID | 属性 |
|---|---|---|
| 服务 | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX 特征 | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response |
| TX 特征 | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

**手机 → ESP32**：写 RX 特征
**ESP32 → 手机**：订阅 TX 特征接收状态通知

> 这组 UUID 是 **Nordic UART Service (NUS)** 兼容格式。
> 很多现成的 BLE 调试 App（nRF Connect、Serial Bluetooth Terminal）
> 能直接识别，你可以在写自己的 App 之前先用它们测试。

---

## 四、手机端怎么发数据

ESP32 支持**三种输入方式，自动识别**，不需要任何配置：

### 方式 0：十六进制文本（最省事，nRF Connect 直接用）

**如果你的 App 输入框只能打文本**（比如 nRF Connect 的写特征界面），
直接在输入框里打：

```
A5 5A 01 00 00 00 74 F2
```

ESP32 会**自动识别这是十六进制文本并转成真正的字节**。

支持的写法（都能正确解析）：

| 写法 | 能否识别 |
|---|---|
| `A5 5A 01 00` | ✓ 空格分隔 |
| `A55A0100` | ✓ 连写 |
| `a5 5a 01 00` | ✓ 小写 |
| `0xA5,0x5A,0x01,0x00` | ✓ 0x 前缀 + 逗号 |
| `A5\t5A\r\n01 00` | ✓ 含换行制表符 |

> **为什么需要这个**：nRF Connect 的写界面默认是文本模式，
> 你打 `A5 5A` 时它会把每个字符按 UTF-8 发出去，
> 变成 23 个字节（`0x41 'A'`, `0x35 '5'`, `0x20 空格`...），
> 而不是 8 个字节（`0xA5`, `0x5A`, ...）。
> 首字节是 `0x41` 而不是 `0xA5`，STM32 会拒绝整帧。
>
> ESP32 现在会自动把这层文本转换掉，所以你**不用换 App、不用找 Hex 模式**。

**怎么确认生效**：看 ESP32 日志会出现

```
I () main: RX 23 bytes: 41 35 20 35 41 20 30 31
I () main: hex text detected (23 chars) -> 8 bytes
I () main: frame passthrough: cmd=0x01 seq=0 len=8
```

三行连起来就说明：收到的是文本 → 转成了 8 字节 → 识别为 PING 帧。

### 方式 A：透传完整协议帧（推荐，自己写 App 时用）

手机端自己实现协议封装，ESP32 原样转发。

**判断依据**：数据以 `A5 5A` 开头（真正的二进制字节，不是文本）。

```python
# Python 示例（手机端逻辑）
def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def build_frame(cmd: int, seq: int, payload: bytes = b'') -> bytes:
    body = bytes([cmd, seq]) + len(payload).to_bytes(2, 'little') + payload
    return b'\xA5\x5A' + body + crc16(body).to_bytes(2, 'little')
```

**发一张 240×240 图**：

```python
seq = 0

# 1. 开始
begin = (240).to_bytes(2,'little') + (240).to_bytes(2,'little') \
      + (0).to_bytes(2,'little') + (0).to_bytes(2,'little') + bytes([0])
ble_write(build_frame(0x10, seq, begin)); seq = (seq+1) & 0xFF

# 2. 数据分片（每片 512 字节，即 256 像素）
for i in range(0, len(rgb565_bytes), 512):
    chunk = rgb565_bytes[i:i+512]
    ble_write(build_frame(0x11, seq, chunk)); seq = (seq+1) & 0xFF

# 3. 结束
ble_write(build_frame(0x12, seq))
```

> `rgb565_bytes` 必须是**大端**（高字节在前），即 UART 上先出现 `color >> 8`。

### 方式 B：裸数据

手机直接发像素数据（**不以 `A5 5A` 开头，也不是十六进制文本**），
ESP32 自动按 512 字节分片成 `IMAGE_DATA`。

**注意**：这种方式需要你先另发一个 `IMAGE_BEGIN` 告知尺寸，
否则 STM32 不知道该收多少像素。所以**方式 A 更可控，推荐用 A**。

### 识别优先级

ESP32 按这个顺序判断（`main.c` 的 `handle_packet()`）：

```
1. 全是十六进制字符和分隔符？  → 当作 hex 文本，先转成字节
        ↓ 不是
2. 首两字节是 A5 5A？          → 当作协议帧，原样透传
        ↓ 不是
3. 其他                        → 当作裸图像数据，分片发
```

> **安全性**：真正的二进制帧不会被误判成文本。
> 因为帧里有 `0xA5`、`0xF2` 这类非 ASCII 字节，
> 预检时会被判定为「含非法字符」而跳过文本解析。
> 这一点有单元测试覆盖（`test/test_hexparse.c`）。

---

## 五、命令速查

完整定义见 STM32 项目 `docs/UART_PROTOCOL.md`。

| CMD | 功能 | 负载 |
|---|---|---|
| `0x01` | PING | 无 |
| `0x10` | IMAGE_BEGIN | w(2) h(2) x(2) y(2) fmt(1)，小端 |
| `0x11` | IMAGE_DATA | 像素数据（高字节在前） |
| `0x12` | IMAGE_END | 无 |
| `0x20` | PLAY | 无 |
| `0x21` | PAUSE | 无 |
| `0x22` | STOP | 无 |
| `0x30` | SET_SCALE | 1 字节：1~4 |
| `0x31` | SET_POS | x(2) y(2)，小端 |
| `0x32` | SET_BRIGHT | 1 字节：0~100 |
| `0x33` | CLEAR | 2 字节颜色，小端 |
| `0x7E` | ACK | error(1) orig_cmd(1) orig_seq(1) |
| `0x7F` | NAK | 同上 |

---

## 六、调试

### 先用 nRF Connect 验证 BLE 通不通

1. 手机装 **nRF Connect**
2. 扫描，找到 `OLCD-ESP32`
3. 连接，展开服务 `6E400001-...`
4. 找到 RX 特征 `6E400002-...`，点上传图标
5. 选 **Hex** 格式，发这 8 个字节：

```
A5 5A 01 00 00 00 74 F2
```

这是 PING 帧。STM32 应该回 ACK。

> 这个帧的 CRC 我已经用与两端完全相同的算法验证过。

### 看 ESP32 日志

```powershell
.\build.ps1 -Monitor
```

正常启动应该看到：

```
I (xxx) main: === ESP32 BLE -> STM32 bridge ===
I (xxx) uart_link: UART2 ready: TX=GPIO17 RX=GPIO16 baud=921600
I (xxx) ble_server: BLE server started
I (xxx) ble_server: advertising as "OLCD-ESP32"
I (xxx) main: ready. Connect with BLE app, then send data.
```

连接成功后：

```
I (xxx) ble_server: phone connected, handle=0
I (xxx) ble_server: MTU updated: 517
```

---

## 七、性能参考

| 项目 | 数值 | 说明 |
|---|---|---|
| 图片大小 | 240×240×2 = **115,200 字节** | 全屏 RGB565 |
| BLE MTU | 517（协商后） | 单个 BLE 包可带 ~512 字节 |
| UART 波特率 | 921600 | 115200 太慢（10 秒） |
| 分片大小 | 512 字节/片 | 对应 256 像素 |
| 分片数 | 约 225 片 | |
| 片间延时 | 2ms | **给 STM32 喘气，不能去掉** |
| BLE 传输耗时 | 约 1~2 秒 | 取决于手机和连接间隔 |
| UART 传输耗时 | 约 1.3 秒 | |
| **总耗时** | **约 2~4 秒/帧** | |

### 为什么片间要延时

STM32 是**软件模拟 SPI** 写屏（不是硬件 SPI），每写一个像素都要
翻转 GPIO，速度有限。它的 UART 接收环形缓冲只有 512 字节。

如果 ESP32 不加延时地猛灌数据，STM32 会来不及处理，
表现为**图像传输到一半就卡住**（缓冲溢出丢数据）。

`CHUNK_GAP_MS = 2` 是保守值。如果实测不丢包，可以调到 1 甚至 0；
如果仍然丢包，加大到 3~5。

---

## 八、代码结构

```
OLCD/
├── CMakeLists.txt          项目根 CMake
├── sdkconfig.defaults      默认配置（BLE/NimBLE 相关）
├── build.ps1               构建脚本（补齐 ESP-IDF 环境变量）
├── main/
│   ├── CMakeLists.txt      组件依赖声明
│   ├── main.c              入口：队列 + 转发任务
│   ├── ble_server.c/h      BLE GATT 服务
│   ├── uart_link.c/h       UART2 发送/接收
│   └── proto.c/h           协议封装（与 STM32 端一致）
└── test/
    └── test_proto.c        PC 端协议验证
```

### 数据流设计

```
BLE 回调（协议栈任务）
    │  只做：拷贝进队列
    ▼
s_rx_queue（32 深度）
    │
    ▼
forward_task（独立任务）
    │  做：判断模式、打包、UART 发送、片间延时
    ▼
UART2 → STM32
```

**为什么用队列解耦**：BLE 回调运行在协议栈任务上下文，
如果在里面直接做 UART 发送（尤其带 2ms 延时 × 225 片 = 450ms），
会阻塞协议栈，导致连接超时断开。

---

## 九、常见问题

### 编译报 `"cmake" must be available on the PATH`

用 `build.ps1`，别直接敲 `idf.py`。原因见第一节。

### 编译报 `TypeError: expected string or bytes-like object, got 'NoneType'`

同上，缺 `ESP_IDF_VERSION`。

### 手机搜不到 `OLCD-ESP32`

**先看日志有没有这一行错误：**

```
E (413) ble_server: adv set fields failed: 4
```

`4` = `BLE_HS_EMSGSIZE`，意思是**广播包超过 31 字节**。这是 BLE 的硬限制。

各字段开销（每个 AD 结构 = 2 字节头 + 数据）：

| 字段 | 字节数 |
|---|---|
| Flags | 3 |
| 128 位 UUID | 18 |
| 设备名 `OLCD-ESP32` | 12 |

如果全塞进广播包：`3 + 18 + 12 = 33`，**超出 2 字节** → 广播直接失败。

**已修复**：现在广播包只放 `Flags + 设备名`（15 字节），
128 位 UUID 移到**扫描响应包**（18 字节），两个包各自都在 31 字节内。
手机主动扫描时会把两个包合并，按 UUID 过滤依然有效。

如果日志显示 `advertising as "OLCD-ESP32"` 但仍搜不到：

- 手机蓝牙是否开启
- 换 nRF Connect 扫描（有些系统蓝牙设置界面不显示 BLE 设备）
- 设备可能已被别的手机连着，先断开

### 启动时有 `spi_flash: Detected size(16384k) larger than ... (2048k)` 警告

你的模块是 **16MB Flash**，但默认配置只认 2MB，14MB 被浪费。

**已修复**：`sdkconfig.defaults` 里改成 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`，
并加了自定义分区表 `partitions.csv`。重新编译后：

| 项目 | 修复前 | 修复后 |
|---|---|---|
| Flash 可用 | 2MB | **16MB** |
| 应用分区 | 1MB | **2MB** |
| 分区剩余 | 54% | **78%** |

> 注意：改了 `sdkconfig.defaults` 后必须**删掉 `sdkconfig` 再重新构建**，
> 否则旧配置会覆盖。`build.ps1` 在 sdkconfig 不存在时会自动 `set-target`。

### 能连上但发数据没反应

- **确认写的是 RX 特征**（`...0002...`），不是 TX 特征
- 看 ESP32 日志有没有收到数据
- 用 nRF Connect 先发 PING 帧测试

### 图像传到一半卡住

- **加大 `CHUNK_GAP_MS`**（在 `main.c` 里，改成 3~5）
- 减小 `FRAME_CHUNK_SIZE`（512 → 256）
- 检查 STM32 端是否在忙别的（比如正在播放 GIF）

### 屏幕显示的颜色不对（红蓝互换）

RGB565 的字节序问题。手机端发的应该是**高字节在前**：

```python
# 正确
data[i]   = (color >> 8) & 0xFF   # 先发高字节
data[i+1] = color & 0xFF

# 错误（会导致红蓝互换）
data[i]   = color & 0xFF
data[i+1] = (color >> 8) & 0xFF
```

### 连接后频繁断开

- 手机离太远
- 检查供电（ESP32 供电不足会重启）
- 看日志有没有 `host reset` 或 brownout 警告

---

## 十、协议一致性保证

ESP32 和 STM32 两端的协议实现必须**逐字节一致**。已验证：

| 验证项 | 结果 |
|---|---|
| CRC16 基准值 `crc16("123456789")` | 两端都是 `0x29B1` ✓ |
| PING 帧字节 | 两端都是 `A5 5A 01 00 00 00 74 F2` ✓ |
| 帧头 / 长度 / 小端布局 | 一致 ✓ |

**改动任何一端的协议定义，必须同步改另一端，否则通讯失败。**

PC 端验证命令：

```powershell
D:\stm32\SPIOLED\tools\tcc\tcc\tcc.exe -I main -o test_proto.exe test\test_proto.c main\proto.c
.\test_proto.exe
```

---

## 十一、依赖与版本

| 项目 | 版本 |
|---|---|
| ESP-IDF | v6.1.0 |
| **目标芯片** | **ESP32-S3**（⚠️ 不是经典 ESP32） |
| 芯片版本 | v0.2 |
| BLE 协议栈 | NimBLE |
| 工具链 | xtensa-esp-elf esp-15.2.0（`xtensa-esp32s3-elf-*`） |
| Flash | 16MB |
| 应用分区 | 2MB |
| 固件大小 | 约 480 KB（分区剩余 77%） |
| 控制台 UART | UART0，GPIO43(TX) / GPIO44(RX) |
| 数据 UART | UART2，GPIO17(TX) / GPIO16(RX) |

组件依赖（ESP-IDF 6.x 的划分，注意 `driver` 已拆分成独立组件）：

```
bt, nvs_flash, esp_driver_uart, esp_driver_gpio, esp_timer
```

**ESP32 vs ESP32-S3 的关键差异**（本项目按 S3 配置）：

| 项目 | ESP32 | ESP32-S3 |
|---|---|---|
| 控制台 UART 引脚 | GPIO1 / GPIO3 | **GPIO43 / GPIO44** |
| `CONFIG_BTDM_CTRL_MODE_*` | 支持 | **不支持** |
| `BT_LE_DFT_TX_POWER_LEVEL` | 无 | **无**（只能用运行时 API） |
| GPIO 范围 | 0~39 | **0~48**（22~25 不可用） |


---

## 十二、已解决的问题记录

这些是实际调试中遇到并修复的坑，记下来避免重复踩。

### 0. ★★★ 帧被 BLE 拆包导致 STM32 完全无响应

**这是最隐蔽的一个问题，表现是"能连上、能发数据，但屏幕一动不动"。**

**现象**：日志里每 20 来个包就出现一组这样的三连：

```
I (20415) main: frame passthrough: cmd=0x11 seq=138 len=509   ← 只有 509 字节
I (20425) main: RX 11 bytes: EE 8B EF 33 CE 8C 4F 8C          ← 缺的 11 字节
W (20425) main: not a frame (head=74 AD), treated as raw image data  ← 被误处理
```

统计一次传输：`frame passthrough` 255 次，`not a frame` **237 次**。

**根因**：两个数字对不上。

| 项目 | 大小 |
|---|---|
| BLE 单包载荷上限（MTU 512 − 3） | **509 字节** |
| 一个 IMAGE_DATA 帧（6 + 512 负载 + 2 CRC） | **520 字节** |

520 > 509，所以手机端的 GATT 栈**必然**把一帧拆成 `509 + 11` 两包。

而 ESP32 原来的逻辑是：**把每个 BLE 包都当成一条独立完整的协议帧**：

- 第 1 包（509 字节）：以 `A5 5A` 开头 → 认作完整帧，原样透传（**但少了 11 字节**）
- 第 2 包（11 字节）：以数据字节开头 → 认不出帧头 → 被当成"裸图像数据"
  → **重新封装成一条新的 IMAGE_DATA 帧**发下去

结果 STM32 收到的是错乱的数据流：

```
[残缺的帧][ESP32 凭空造的帧][残缺的帧][凭空造的帧]...
```

STM32 的 CRC 校验大面积失败，每一帧都被丢弃 → **屏幕完全不动**。

**修复**：在 ESP32 侧加入**流式帧重组**（`main.c` 的 `feed_accum()`）。

思路是按协议自己算帧边界：
```
收到 >= 6 字节  → 读 LEN 字段 → 总长 = 6 + LEN + 2
累积够总长     → 完整帧，透传
不够          → 留在缓冲里等下一包
```

关键代码：

```c
payload_len = p[4] | (p[5] << 8);           /* 小端 */
total = PROTO_HEADER_SIZE + payload_len + PROTO_TAIL_SIZE;
if (avail < total) break;                    /* 没齐，等下一包 */
process_frame(p, total);                     /* 齐了，透传 */
```

**回归测试**：`test/test_reasm.c`，21 项检查全部通过，覆盖：
- 核心场景 509+11 → 拼回 520
- 一包内多个小帧
- 负载内部恰好含 `A5 5A`（不能误判为帧头）
- 前导垃圾字节
- 半帧卡住后补齐
- 帧边界不对齐（一包含 1.5 个帧）
- LEN 字段非法时的重同步

**教训**：BLE 的 MTU 是**传输层**约束，协议帧是**应用层**概念，
两者长度不匹配时，**必须在桥接端做重组**，不能假设"一个 BLE 包 = 一个协议帧"。

> 顺带一提：如果不想做重组，还有个更简单的规避办法——
> 让手机端把 IMAGE_DATA 的负载降到 **500 字节**，
> 这样一帧 = 6+500+2 = 508 < 509，正好塞进一个 BLE 包。
> 但这是治标的，遇到更大 MTU 或更小帧长时仍会出问题，
> 所以最终选了重组方案。

---

### 0.1 Brownout 复位（BLE 射频启动时掉电重启）

**现象**：串口日志打到一半突然重启，反复循环：

```
I (400) BLE_INIT: BT controller compile version [51d9dfd]
I (410) phy_init: phy_version 712,87e8c20e,Apr 13 2026,18:51:10
E BOD: Brownout detector was triggered        ← 关键
```

配上复位原因：

```
rst:0xf (BROWNOUT_RST)
Saved PC: 0x40376998  (rtc_brownout_isr_handler at brownout.c:82)
```

**原因**：BLE 射频刚启动瞬间（`phy_init`）电流从 40mA 突增到
**300~500mA**。供电能力不足时电压瞬间跌破阈值，芯片保护性复位。

**这不是软件 bug，是供电问题。**

---

#### ★ 踩过的坑 1：brownout 档位方向搞反了

**曾把档位从默认 7 改成 3，以为是在"降低阈值"，实际完全相反。**

`esp32s3/Kconfig.power` 第 25-38 行的真实映射：

| 档位 | 触发电压 | 说明 |
|---|---|---|
| **7（官方默认）** | **2.44V** | 最低阈值，**最宽容** |
| 6 | 2.56V | |
| 5 | 2.67V | |
| 4 | 2.84V | |
| **3** | **2.98V** | ← 曾误设成这个 |
| 2 | 3.19V | |
| 1 | 3.30V | 最高阈值，最敏感 |

从 7 改到 3，等于把触发电压从 **2.44V 提高到 2.98V**，
容忍度下降 0.54V，**brownout 反而更容易发生**。

**正确配置（已恢复）**：

```ini
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y
CONFIG_ESP_BROWNOUT_DET_LVL=7
```

> 记住：**数字越大越宽容**。不要调小。

---

#### ★ 踩过的坑 2：降功率代码从未执行过

`ble_server.c` 里确实写了 `esp_ble_tx_power_set()`，但日志里
**从来没有出现过**这行：

```
I () ble_server: BLE TX power set to 0dBm
```

原因在 `nimble_port_init()` 的内部实现：

```c
esp_err_t nimble_port_init(void) {
    esp_bt_controller_init(&config_opts);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);   // ← 射频在这里就启动了
    esp_nimble_init();
    return ESP_OK;                                // ← 这时才返回
}
```

射频在 `esp_bt_controller_enable()` 那一刻就启动了，
而 `esp_ble_tx_power_set()` 写在 `nimble_port_init()` 返回**之后**，
**根本没机会执行**。

**并且 ESP32-S3 上无法在射频启动前设功率**（已核实）：

- `esp_bt_controller_config_t` 结构体（`esp_bt.h` 第 274-344 行）
  **没有任何功率字段**
- `BT_LE_DFT_TX_POWER_LEVEL` 这个 Kconfig 只存在于
  C2/C5/C6/H2，**S3 没有**
- 日志里**没有** `BLE TX power set to 0dBm` 就是证据

**结论：这条路走不通，软件救不了启动瞬间的冲击。**

---

#### ★ 踩过的坑 3：改了 sdkconfig.defaults 却没生效

规则是「**`sdkconfig` 存在时忽略 `sdkconfig.defaults`**」。

如果只改 defaults 而 `sdkconfig` 已存在，改动**不会进入固件**。

**判断方法**：看串口日志里的 `compile time`。
如果不是你刚编译的时间，烧的就是旧固件。

```
I (25) boot: compile time Nov 25 2026 18:52:19   ← 旧固件！
```

**正确做法**：

```powershell
Remove-Item sdkconfig
.\build.ps1 -Flash
```

---

#### 软件层面的功耗优化（已实施，有效但不是决定性的）

| 配置 | 默认 | 现在 | 效果 |
|---|---|---|---|
| CPU 主频 | 240 MHz | **160 MHz** | 降 20~30% 功耗 |
| Flash 频率 | 80 MHz | **40 MHz** | 降低读取峰值 |
| brownout 档位 | 7 | **7** | 恢复默认最宽容 |

```ini
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
CONFIG_ESPTOOLPY_FLASHFREQ="40m"
```

---

#### 硬件解决（决定性，按有效性排序）

**1. 换 USB 线和接口**

```
✗ 细线、长线、前面板 USB、USB Hub
✓ 粗短线、机箱后置 USB 口、优先蓝色 USB 3.0
```

**两个 USB 口都插电脑 ≠ 功率够用**：

- USB 2.0 口标称 500mA 只是上限，前面板/经 Hub 更低
- 两块板插同一个电脑，很可能共享同一路 5V 供电轨
- 线材内阻 0.5~1Ω，500mA 时压降 0.25~0.5V
- AMS1117 需要压差，输入 4.7V 时输出最多 3.6V
- 两板共 GND 又各接电脑，形成地环路

**2. 分开供电（最可靠）**

```
ESP32  ← 电脑 USB（需要烧录和串口）
STM32  ← 手机充电头 / 移动电源 / ST-Link
两者只连 GND
```

让 ESP32 独占一路供电。

**3. 加电容**

在 ESP32 的 **3V3 与 GND 之间**并：

```
100µF 电解电容 + 0.1µF 陶瓷电容
```

吸收射频启动的微秒级电流冲击，这是解决瞬态掉电最经典的手段。

**4. 降低屏幕背光**

背光是 STM32 侧主要负载。`LCD_SetBrightness(100u)` 改成 `40u` 可省 30~50mA。

---

#### 排查顺序（推荐）

```
1. 单接 ESP32，不接 STM32
   → 能跑到 advertising as "OLCD-ESP32" 才算电源够

2. 换粗短线 + 机箱后置 USB 3.0 口

3. 分开供电（ESP32 独占一路）

4. 加 100µF + 0.1µF 电容

5. 最后才考虑降屏幕背光
```

> 如果第 7 档（2.44V，最低阈值）仍然 brownout，
> 说明供电确实严重不足，**软件已到极限，必须改硬件**。

### 1. 广播包超长导致手机搜不到设备

**现象**：日志出现 `E ble_server: adv set fields failed: 4`，手机扫描不到设备。

**原因**：BLE 广播包上限 31 字节。原代码把
`Flags(3) + 128位UUID(18) + 设备名(12) = 33 字节` 全塞进广播包，超了 2 字节。

**修复**：拆分成两个包
- 广播包：`Flags + 设备名` = 15 字节
- 扫描响应包：`128 位 UUID` = 18 字节

**关键代码**（`ble_server.c` 的 `ble_advertise()`）：
```c
ble_gap_adv_set_fields(&adv_fields);        // 广播包
ble_gap_adv_rsp_set_fields(&rsp_fields);    // 扫描响应包
ble_gap_adv_start(...);
```

### 2. Flash 容量未充分利用

**现象**：启动警告 `Detected size(16384k) larger than the size in the
binary image header(2048k)`。

**原因**：模块是 16MB Flash，ESP-IDF 默认按 2MB 配置。

**修复**：`sdkconfig.defaults` 中设 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`，
并添加自定义分区表 `partitions.csv`（应用分区扩到 2MB，另留 2 个 OTA 分区
和 10MB storage 区）。

**注意**：改了 `sdkconfig.defaults` 必须删掉 `sdkconfig` 重新构建，
否则旧配置不生效。

### 3. ESP-IDF 6.x 的组件依赖变化

**现象**：编译报 `fatal error: driver/uart.h: No such file or directory`。

**原因**：ESP-IDF 6.x 把 `driver` 拆成了多个独立组件。

**修复**：`main/CMakeLists.txt` 的 `REQUIRES` 改成：
```cmake
esp_driver_uart esp_driver_gpio
```
（而不是笼统的 `driver`）

### 4. EIM 安装的 ESP-IDF 无法直接运行 idf.py

**现象**：
- `TypeError: expected string or bytes-like object, got 'NoneType'`
- `"cmake" must be available on the PATH`

**原因**：EIM 的激活脚本没设 `ESP_IDF_VERSION`，也没把工具链加进 PATH。

**修复**：用 `build.ps1`，它会补齐这些环境变量。

### 5. ★ build.ps1 默认 target 是 esp32（错误芯片）

**这是一个已经修掉、但很值得记住的定时炸弹。**

原脚本第 23 行：

```powershell
[string]$Target = "esp32"     # 错！实际芯片是 esp32s3
```

触发条件：
1. 删除 `sdkconfig`（比如为了让 `sdkconfig.defaults` 生效）
2. 运行 `.\build.ps1`

此时脚本会执行 `idf.py set-target esp32`，
**编译出普通 ESP32 固件，烧到 ESP32-S3 上完全无法启动**。

更糟的是：现象很像硬件故障（连 bootloader 日志都不对），
会让人往供电、烧录器等方向排查，浪费大量时间。

**修复**：

1. 默认值改成 `esp32s3`
2. 增加一致性校验：`sdkconfig` 已存在时，解析其中的
   `CONFIG_IDF_TARGET` 与脚本期望值比对，不一致就**拒绝构建**并给出提示

```
==========================================================
 错误：目标芯片不匹配
==========================================================
 现有 sdkconfig 记录的芯片 : esp32
 本脚本期望的芯片         : esp32s3
 继续构建会得到错误芯片的固件，烧录后无法启动。
```

### 6. build.ps1 把 CMake 警告误判为失败

**现象**：构建实际成功（`Project build complete`、`exit=0`），
但脚本报 `NativeCommandError` 并退出 1。

**原因**：ESP-IDF 构建时会向 stderr 打印大量 CMake Warning，例如：

```
Private include directory '...esp_wifi/...' belongs to component esp_wifi
but is being used by component wpa_supplicant
```

这些都是 IDF 内部的组件引用警告，**不影响编译结果**。
但脚本里的 `$ErrorActionPreference = "Stop"` 会把任何 stderr 输出
当成终止性错误。

**修复**：`Invoke-Idf` 函数里临时把 `ErrorActionPreference`
改成 `Continue`，只以 `idf.py` 的真实退出码为准。

### 7. PowerShell 脚本里的中文导致解析崩溃

**现象**：`.\build.ps1` 报 `Missing expression after ','`、
`Unexpected token ')'` 等语法错误，但文件内容看起来完全正常。

**原因**：Windows PowerShell 5.1 读取**无 BOM 的 UTF-8** 文件时
会按 ANSI/GBK 解码。如果文件里有中文注释或 `⚠`（U+26A0）等字符，
字节序列会被错误解析，破坏语法结构。

**修复**：
- 把脚本保存为 **UTF-8 with BOM**（`EF BB BF`）
- 避免在脚本里使用 emoji / 特殊符号
- 用 `pwsh` 或 `[System.IO.File]::WriteAllText` 配合
  `New-Object System.Text.UTF8Encoding $true` 写文件

> 注意：很多编辑器（包括某些自动化工具）默认存成无 BOM 的 UTF-8，
> 每次改动后都要检查 BOM 是否存在。

#   E S P 3 2 L C D  
 