# remoe

`remoe_host` 是 Windows 桌面视频流 host 原型：使用 Desktop Duplication API 抓取指定显示器，
通过 NVIDIA NVENC 编码为低延迟 AV1，并通过 TCP 向一个 client 发送码流。

`remoe_client` 使用 Intel oneVPL 和 D3D11 视频内存进行 AV1 硬件解码及显示。解码画面不会逐帧
回读到 CPU，呈现使用垂直同步和单帧队列，以降低 Intel GPU、CPU 与显示链路的额外功耗。

当前版本提供画面传输和窗口内的键鼠远程控制。音频、剪贴板、文件传输、加密与身份验证尚未实现。

## 环境要求

- Windows 10/11 x64
- Visual Studio 2022（Desktop development with C++）及 Windows 10/11 SDK
- CMake 3.24+
- 支持 AV1 NVENC 的 NVIDIA GPU 和兼容驱动（通常为 Ada Lovelace 或更新架构）
- NVIDIA Video Codec SDK 13.0.37，默认路径：
  `third_party/NVENC_Video_Codec_SDK_13.0.37`
- libdatachannel 0.24.5 及其固定版本子依赖，默认路径：
  `third_party/libdatachannel-0.24.5`
- Mbed TLS 3.6.7 源码，默认路径：`third_party/mbedtls-3.6.7`
- 构建 client 时需要 Intel oneVPL 2.x 源码，默认路径：`third_party/libvpl`
- 运行 client 时需要支持 AV1 硬件解码的 Intel GPU 和当前 Intel 图形驱动

当前工程固定使用 NVENC API 13.0，以兼容本机驱动 596.49。若替换 SDK，所用 SDK 的 NVENC API
版本不能高于驱动通过 `NvEncodeAPIGetMaxSupportedVersion` 返回的版本。

## 构建

推荐在普通 PowerShell 或命令提示符中使用自动构建脚本。它会查找最新安装的 Visual Studio
（2022 或更新版本）、加载 x64 MSVC 环境，并使用 Ninja Multi-Config 构建。这也适用于 CMake
尚未提供对应 Visual Studio 生成器的新版 Visual Studio：

```powershell
.\build.cmd
# 或构建其他配置
.\build.cmd Debug
```

默认生成文件为 `build-local/Release/remoe_host.exe` 和同目录的 `datachannel.dll`。脚本要求 Visual Studio 安装了
“Desktop development with C++”，并且 PATH 中存在 Ninja；Visual Studio 的 C++ CMake tools
组件或单独安装的 Ninja 均可。

当 `third_party/libvpl` 存在时还会生成 `build-local/Release/remoe_client.exe`。如果 oneVPL
源码不在默认位置，可在手动配置 CMake 时传入 `-DVPL_ROOT="D:/path/to/libvpl"`。依赖缺失时
原有的 host-only 构建仍然可用。

libdatachannel 使用随项目编译的 Mbed TLS 静态加密后端，因此不要求开发机额外安装 OpenSSL SDK。
媒体传输、DataChannel 和 WebSocket 支持均已启用；当前 TCP 业务路径暂未切换到 WebRTC。

原有的 Visual Studio 生成器方式仍然支持。在 “Developer PowerShell for VS 2022” 中执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

若 SDK 位于其他路径：

```powershell
cmake -S . -B build -A x64 -DNVENC_SDK_ROOT="D:/path/to/Video_Codec_SDK"
cmake --build build --config Release
```

生成文件为 `build/Release/remoe_host.exe`。

## 运行

`remoe_host.exe` 默认以当前用户权限运行，不会主动弹出 UAC。需要管理员权限时添加 `--admin`，
程序会使用 Windows `runas` 重新启动自身；如果用户拒绝授权，host 不会运行。管理员权限可以减少
普通高完整性窗口的交互限制，但 Windows UAC 安全桌面仍属于隔离桌面，当前版本不能采集或远程操作
安全桌面本身。

仅本机监听（默认且更安全，同时监听 `127.0.0.1` 和 `::1`）：

```powershell
.\build\Release\remoe_host.exe
```

以管理员权限启动：

```powershell
.\build\Release\remoe_host.exe --admin
```

允许局域网 client 连接：

```powershell
.\build-local\Release\remoe_host.exe --bind "*" --port 47990
```

参数：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `--bind` | `localhost` | TCP 监听地址；默认同时监听 IPv4/IPv6 loopback。`*`、`0.0.0.0` 或 `::` 表示同时监听 IPv4/IPv6 任意地址；具体 IP 只监听该地址族 |
| `--port` | `47990` | TCP 端口 |
| `--output` | `0` | 显示器索引（跨所有 DXGI adapter 顺序编号） |
| `--max-fps` | 不限制 | 可选的 client 最大帧率 |
| `--max-bitrate` | 不限制 | 可选的 client 最大 CBR 码率，单位 Mbps |
| `--admin` | 关闭 | 通过 UAC `runas` 重新启动为管理员进程 |

兼容原有启动脚本，`--fps` 和 `--bitrate` 分别作为以上两个上限的别名继续接受。实际编码参数由
每次连接的 client 请求决定；host 不带这些参数启动时不额外设置上限，显式设置后超出上限的请求会被拒绝。

Windows Defender Firewall 首次运行可能提示放行。只应在可信网络中放行“专用网络”，不要直接暴露到公网。

## Client 运行

连接远端 host：

```powershell
.\build-local\Release\remoe_client.exe --host 10.14.178.25 --port 47990 `
  --fps 60 --bitrate 20 --scale 75
```

client 强制要求 Intel AV1 D3D11 硬件解码。如果没有匹配的 Intel GPU、驱动或 oneVPL runtime，
会直接报错而不会回退到软件解码。客户端窗口获得焦点后，窗口内的鼠标和键盘操作会发送给 host；
窗口失焦、关闭或连接断开时会释放仍按下的远端按键和鼠标按钮。关闭播放窗口即可断开连接。

窗口标题每秒更新一次实际 AV1 payload 码率和应用层 TCP 接收速度。前者以 Mbps 显示，后者以 MB/s
显示，不包含 TCP/IP 和链路层包头。

网络接收和 oneVPL 解码在独立线程运行，中间使用约两秒、8–64 MB 的有界队列吸收短暂的 GPU
解码或 VSync 停顿。若客户端持续跟不上，接收线程会清除旧 GOP，并请求 host 立即生成关键帧，
随后重建解码器恢复到最新画面，避免解码反压直接造成 TCP 半帧断线。正常播放使用无限 GOP，
不会因固定间隔关键帧在低码率下产生周期性的画质波动。

`--fps` 可选范围为 1–240；`--bitrate` 是画质控制参数，单位 Mbps，可选范围为 1–1000；
`--scale` 是 host 编码分辨率相对被捕获显示器的百分比，可选范围为 10–100，默认 100。例如源画面
为 2560×1440 时，`--scale 75` 会请求 1920×1080。最终宽高会向下对齐到偶数，并由
`StreamHeader` 回传。缩放在 host 的 D3D11 GPU 视频处理器中完成，不回读 CPU。

## TCP 协议 v5

所有整数都是 **little-endian**，结构紧密排列（无 padding）。连接建立后，client 先发送一次
`ClientConfig`。host 验证并应用请求后发送 `StreamHeader`，随后在 host→client 方向重复发送
`FrameHeader + AV1 payload`，client→host 方向可发送定长 `InputEvent`。每个 payload 是 NVENC
返回的一次编码输出，不是 IVF 容器；client 应将 payload 按顺序提交给 AV1 decoder。

### ClientConfig（28 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMCF` |
| 4 | u16 | version | `5` |
| 6 | u16 | header_size | `28` |
| 8 | u32 | fps_num | client 请求的帧率分子 |
| 12 | u32 | fps_den | 帧率分母，当前必须为 1 |
| 16 | u32 | bitrate_bps | client 请求的 CBR 码率 |
| 20 | u32 | scale_percent | client 请求的编码分辨率百分比，10–100 |
| 24 | u32 | reserved | 0 |

### StreamHeader（36 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMOE` |
| 4 | u16 | version | `5` |
| 6 | u16 | header_size | `36` |
| 8 | u32 | codec | `AV01` |
| 12 | u32 | width | 编码宽度 |
| 16 | u32 | height | 编码高度 |
| 20 | u32 | fps_num | 帧率分子 |
| 24 | u32 | fps_den | 帧率分母，当前为 1 |
| 28 | u32 | bitrate_bps | 目标码率 |
| 32 | u32 | reserved | 0 |

### FrameHeader（32 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `FRAM` |
| 4 | u16 | version | `5` |
| 6 | u16 | header_size | `32` |
| 8 | u32 | payload_size | 后续 AV1 数据长度 |
| 12 | u32 | flags | bit 0 = key frame；bit 1 预留为 codec config |
| 16 | u64 | frame_number | 递增编号 |
| 24 | u64 | timestamp_us | host 启动后的单调时钟微秒数 |

### InputEvent（24 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `INPT` |
| 4 | u16 | version | `5` |
| 6 | u16 | header_size | `24` |
| 8 | u16 | type | 1=移动；2–6=左/右/中/X1/X2；7/8=垂直/水平滚轮；9=键盘；10=请求关键帧 |
| 10 | u16 | flags | bit 0=释放；bit 1=扩展扫描码 |
| 12 | i32 | value1 | 移动 X（0–65535）、滚轮 delta 或 Windows 扫描码 |
| 16 | i32 | value2 | 移动 Y（0–65535），其余类型为 0 |
| 20 | u32 | sequence | client 递增事件编号 |

type 10 是控制消息而不是键鼠输入，其 flags、value1、value2 必须为 0。host 收到后会在下一张
捕获画面强制生成携带 sequence header 的 AV1 IDR，用于客户端丢弃积压帧后的快速恢复。

鼠标坐标相对实际视频区域归一化，窗口宽高比不同产生的黑边不参与映射；拖动越过视频边缘时坐标
会夹到边缘。host 将坐标映射回被捕获的 DXGI output，因此也支持位于负坐标的副显示器。

client 连接后的第一张输入强制为 IDR/key frame，并请求 NVENC 携带 sequence header，因而 client 无需
连接建立前的码流状态。断线后 host 返回监听状态，支持后续 client 重连；同一时刻只服务一个 client。

## 当前限制与安全边界

- 协议目前没有 TLS 或认证。默认仅监听 `127.0.0.1` 和 `::1`；若绑定局域网地址，任何可访问该端口的设备都
  可能看到桌面并注入键鼠操作。只应在可信隔离网络中使用，正式使用前应加入 TLS 和配对认证。
- Desktop Duplication API 不会自动把硬件鼠标指针合成到桌面纹理，当前画面可能不显示鼠标指针。
- 锁屏、UAC 安全桌面、显示模式切换和部分受保护内容不能正常捕获。
- `SendInput` 受 Windows UIPI 限制。控制高完整性应用时 host 通常也需要 `--admin`；即使提升权限，
  `Ctrl+Alt+Del` 和 UAC 安全桌面仍不能通过普通 `SendInput` 控制。
- client 可按百分比请求编码缩放，但当前不支持独立指定宽高；显示模式变化后仍需要重启 host。
- 使用原始 TCP，拥塞时可能增加延迟；后续可替换为带拥塞控制的传输层。

## 源码结构

- `src/desktop_capture.*`：DXGI adapter/output 选择与 Desktop Duplication
- `src/main.cpp`：NVENC AV1 配置、采集/编码循环、键鼠注入与重连逻辑
- `src/tcp_server.*`：WinSock TCP server
- `src/protocol.h`：后续 client 共用的 wire protocol 定义
- `src/client_main.cpp`：client 网络接收、协议校验与播放线程
- `src/vpl_decoder.*`：Intel oneVPL AV1 D3D11 硬件解码
- `src/video_window.*`：D3D11 Video Processor、flip-model 窗口呈现与客户端输入采集

第三方 NVIDIA 示例封装源码直接从 SDK 路径参与构建，没有复制或修改 SDK 文件。
