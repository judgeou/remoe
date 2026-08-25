# remoe

`remoe_host` 是 Windows 桌面视频流 host 原型：使用 Desktop Duplication API 抓取指定显示器，
通过 NVIDIA NVENC 编码为低延迟 AV1，并通过 WebRTC 向一个 client 发送码流。视频、键鼠控制和
关键帧请求均使用 DataChannel，不需要知道或开放 host IP/端口。

`remoe_client` 使用 Intel oneVPL 和 D3D11 视频内存进行 AV1 硬件解码及显示。解码画面不会逐帧
回读到 CPU，呈现使用垂直同步和单帧队列，以降低 Intel GPU、CPU 与显示链路的额外功耗。

当前版本提供加密的画面传输和窗口内键鼠远程控制。音频、剪贴板、文件传输与身份验证尚未实现。

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

默认生成文件为 `build-local/Release/remoe_host.exe`；启用 oneVPL 客户端时还会生成
`remoe_client.exe`。libdatachannel 及其加密、SCTP/SRTP 依赖均静态链接，不需要随程序分发
`datachannel.dll`。脚本要求 Visual Studio 安装了
“Desktop development with C++”，并且 PATH 中存在 Ninja；Visual Studio 的 C++ CMake tools
组件或单独安装的 Ninja 均可。

当 `third_party/libvpl` 存在时还会生成 `build-local/Release/remoe_client.exe`。如果 oneVPL
源码不在默认位置，可在手动配置 CMake 时传入 `-DVPL_ROOT="D:/path/to/libvpl"`。依赖缺失时
原有的 host-only 构建仍然可用。

libdatachannel 使用随项目编译的 Mbed TLS 静态加密后端，因此不要求开发机额外安装 OpenSSL SDK。
WSS 连接会将 Windows 当前用户和本机的 `ROOT` 证书库导出给 Mbed TLS，用于验证服务器证书链及
域名；证书无效时连接会直接失败，不会退化为跳过验证。
`src/webrtc_transport.*` 提供 host/client 共用的双 DataChannel 传输封装。可靠有序的 control channel
承载参数协商和键鼠输入；无序、不重传的 video channel 承载 AV1 分片。SDP/ICE 经 WSS 中继，
STUN 地址由信令 URL 自动派生，TURN 有意禁用。

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

启动 host（信令 URL 必填）：

```powershell
.\build-local\Release\remoe_host.exe `
  --signal-url "wss://signal.example.com/signal"
```

参数：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `--signal-url` | 必填 | WSS 信令服务基础 URL；host 会注册到账号设备列表 |
| `--output` | `0` | 显示器索引（跨所有 DXGI adapter 顺序编号） |
| `--max-fps` | 不限制 | 可选的 client 最大帧率 |
| `--max-bitrate` | 不限制 | 可选的 client 最大 CBR 码率，单位 Mbps |
| `--admin` | 关闭 | 通过 UAC `runas` 重新启动为管理员进程 |
| `--repair` | 关闭 | 在 Host 本机重新生成配对码、转移账号并轮换设备凭证 |
| `--legacy-invite` | 关闭 | 兼容模式：自动生成并打印匿名邀请 URL |

兼容原有启动脚本，`--fps` 和 `--bitrate` 分别作为以上两个上限的别名继续接受。实际编码参数由
每次连接的 client 请求决定；host 不带这些参数启动时不额外设置上限，显式设置后超出上限的请求会被拒绝。

WebRTC 会为 ICE 使用本机随机 UDP 端口；Windows Defender Firewall 首次运行可能提示放行。

首次运行时 Host 会打印十分钟有效的八字符配对码。在网页创建或登录 passkey 账号，输入配对码和
设备名称后，Host 会出现在“我的电脑”列表。长期设备凭证使用当前 Windows 用户的 DPAPI 加密，保存到
`%LOCALAPPDATA%\remoe\host-identity.bin`；后续启动不再配对。凭证丢失、系统重装或需要转移账号时，
在 Host 本机添加 `--repair` 再配对即可，旧账号会立即失去这台 Host。

## 浏览器 Client 运行

打开部署好的 HTTPS 网页，通过 passkey 登录后，在在线 Host 旁点击“连接”。服务器会在内部创建一次
短期 Nano ID session 并自动分配给 Browser 与 Host，用户不需要复制 Host IP、session 或邀请 URL。
首次创建账号以及使用恢复码恢复账号时，页面会显示一串只显示一次的轮换恢复码；如果恢复码也丢失，
仍可接触 Host 后使用 `--repair` 绑定到新账号。

## 原生 Client 兼容模式

原生 client 暂时继续支持匿名邀请 URL，用于兼容和诊断。Host 需显式添加 `--legacy-invite`，然后把
它打印的 URL 交给 client；两端都不需要 `--host` 或 `--port`：

```powershell
.\build-local\Release\remoe_host.exe `
  --signal-url "wss://signal.example.com/signal" --legacy-invite

.\build-local\Release\remoe_client.exe `
  --signal-url "wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT" `
  --fps 60 --bitrate 20 --scale 75
```

client 强制要求 Intel AV1 D3D11 硬件解码。如果没有匹配的 Intel GPU、驱动或 oneVPL runtime，
会直接报错而不会回退到软件解码。客户端窗口获得焦点后，窗口内的鼠标和键盘操作会发送给 host；
窗口失焦、关闭或连接断开时会释放仍按下的远端按键和鼠标按钮。关闭播放窗口即可断开连接。

账号管理 Host 默认不再打印邀请 URL。网页中的
“使用临时邀请 URL”折叠入口也只用于兼容测试。错误或过期邀请会立即返回
`Invite not found or expired`。

WebSocket 信令模式会从信令 URL 自动派生同域名的 `stun:<host>:3478` ICE server，不需要额外参数。
STUN 只生成服务器反射地址，不启用 TURN relay；生成 `srflx` candidate 时 host/client 会打印确认信息。

窗口标题每秒更新一次实际 AV1 payload 码率和应用层 WebRTC 接收速度。前者以 Mbps 显示，后者以
MB/s 显示，不包含 UDP/IP 和链路层包头。

网络接收和 oneVPL 解码在独立线程运行，中间使用约两秒、8–64 MB 的有界队列吸收短暂的 GPU
解码或 VSync 停顿。若客户端持续跟不上，接收线程会清除旧 GOP，并请求 host 立即生成关键帧，
随后重建解码器恢复到最新画面。正常播放使用无限 GOP，
不会因固定间隔关键帧在低码率下产生周期性的画质波动。

`--fps` 可选范围为 1–240；`--bitrate` 是画质控制参数，单位 Mbps，可选范围为 1–1000；
`--scale` 是 host 编码分辨率相对被捕获显示器的百分比，可选范围为 10–100，默认 100。例如源画面
为 2560×1440 时，`--scale 75` 会请求 1920×1080。最终宽高会向下对齐到偶数，并由
`StreamHeader` 回传。缩放在 host 的 D3D11 GPU 视频处理器中完成，不回读 CPU。

## WebRTC 传输协议 v7

仓库中的 `web-client/` 是 Chromium 优先的浏览器客户端。它使用 passkey 账号设备列表、STUN-only ICE、
双 DataChannel 和 protocol v7，通过 WebCodecs 解码 NVENC AV1，并发送键鼠 `InputEvent`。连接后
画面自动占满网页；根据浏览器安全规则，用户需点击画面一次才能启用 Pointer Lock。生产部署与
使用方法见 `docs/signaling-server-deployment.md`。

网页使用 Vite、Vue 3 和 TypeScript 构建。开发与生产构建命令：

```bash
cd web-client
npm ci
npm test
npm run build
```

生产文件输出到 `web-client/dist/`；该目录不提交 Git。

所有整数都是 **little-endian**，结构紧密排列（无 padding）。WSS 只交换 SDP/ICE bootstrap 帧；
PeerConnection 建立后不再依赖信令服务器传输业务数据。可靠有序的 `remoe-control` DataChannel
依次承载 `ClientConfig`、`StreamHeader`、`StreamReady` 和 `InputEvent`。无序、不重传的
`remoe-video` DataChannel 承载 `VideoChunkHeader + AV1 chunk`。

### ClientConfig（28 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMCF` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `28` |
| 8 | u32 | fps_num | client 请求的帧率分子 |
| 12 | u32 | fps_den | 帧率分母，当前必须为 1 |
| 16 | u32 | bitrate_bps | client 请求的 CBR 码率 |
| 20 | u32 | scale_percent | client 请求的编码分辨率百分比，10–100 |
| 24 | u32 | flags | 当前必须为 0 |

### StreamHeader（36 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMOE` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `36` |
| 8 | u32 | codec | `AV01` |
| 12 | u32 | width | 编码宽度 |
| 16 | u32 | height | 编码高度 |
| 20 | u32 | fps_num | 帧率分子 |
| 24 | u32 | fps_den | 帧率分母，当前为 1 |
| 28 | u32 | bitrate_bps | 目标码率 |
| 32 | u32 | reserved | 0 |

### StreamReady（8 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `SRDY` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `8` |

client 完成解码队列和窗口初始化后发送此消息；host 收到后才开始发送视频。

### VideoChunkHeader（36 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `VCHK` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `36` |
| 8 | u32 | flags | bit 0 = key frame；bit 1 预留为 codec config |
| 12 | u64 | frame_number | 递增帧编号 |
| 20 | u64 | timestamp_us | host 启动后的单调时钟微秒数 |
| 28 | u32 | frame_size | 完整 AV1 帧长度 |
| 32 | u32 | chunk_offset | 此分片在帧内的字节偏移 |

每条 video DataChannel 消息最多携带 16 KiB AV1 数据。client 按帧号和偏移重组，丢弃长期不完整的帧
并请求新关键帧。AV1 数据是 NVENC 的一次编码输出，不是 IVF 容器。

### InputEvent（24 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `INPT` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `24` |
| 8 | u16 | type | 1=移动；2–6=左/右/中/X1/X2；7/8=垂直/水平滚轮；9=键盘；10=请求关键帧 |
| 10 | u16 | flags | bit 0=释放；bit 1=扩展扫描码 |
| 12 | i32 | value1 | 移动 X（0–65535）、滚轮 delta 或 Windows 扫描码 |
| 16 | i32 | value2 | 移动 Y（0–65535），其余类型为 0 |
| 20 | u32 | sequence | client 递增事件编号 |

type 10 是控制消息而不是键鼠输入，其 flags、value1、value2 必须为 0。host 收到后会在下一张
捕获画面强制生成携带 sequence header 的 AV1 IDR，用于客户端丢弃积压帧后的快速恢复。

### WebRtcSignalHeader（20 bytes，仅连接初始化阶段）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `WRMS` |
| 4 | u16 | version | `7` |
| 6 | u16 | header_size | `20` |
| 8 | u16 | type | 1=SDP；2=ICE candidate；3=DataChannel ready；4=完成确认 |
| 10 | u16 | reserved | 0 |
| 12 | u32 | value_size | 后续 value 长度 |
| 16 | u32 | metadata_size | value 后的 metadata 长度 |

type 1 的 value/metadata 分别是 SDP 和 `offer`/`answer`；type 2 分别是 candidate 和 `mid`；
type 3/4 不带 payload。双方完成确认后，WSS 不再参与业务数据传输。

鼠标坐标相对实际视频区域归一化，窗口宽高比不同产生的黑边不参与映射；拖动越过视频边缘时坐标
会夹到边缘。host 将坐标映射回被捕获的 DXGI output，因此也支持位于负坐标的副显示器。

client 连接后的第一张图像强制为 IDR/key frame，并请求 NVENC 携带 sequence header，因而 client 无需
连接建立前的码流状态。断线后 host 返回监听状态，支持后续 client 重连；同一时刻只服务一个 client。

## 当前限制与安全边界

- 网页账号使用 WebAuthn/passkey 与浏览器会话 Cookie；credential ID 长期保存在浏览器本地，用于兼容
  只能创建不可发现 WebAuthn 凭据的设备。Host 使用 DPAPI 保护的长期 bearer token。
  该简化模型信任服务器，登录 Cookie 或 Host token 被当前用户上下文中的恶意程序窃取时仍可能被冒用。
- 恢复码是约 130 bit 熵的单次 bearer secret，服务端只保存哈希；使用后会在新 passkey 注册成功的
  同一数据库事务中轮换。全部账号凭据丢失时可在 Host 本机重新配对，损失仅为原账号的 Host 列表。
- 自动配置 STUN，但有意禁用 TURN；对称 NAT、UDP 被封锁等无法直连的网络仍可能连接失败。
- Desktop Duplication API 不会自动把硬件鼠标指针合成到桌面纹理，当前画面可能不显示鼠标指针。
- 锁屏、UAC 安全桌面、显示模式切换和部分受保护内容不能正常捕获。
- `SendInput` 受 Windows UIPI 限制。控制高完整性应用时 host 通常也需要 `--admin`；即使提升权限，
  `Ctrl+Alt+Del` 和 UAC 安全桌面仍不能通过普通 `SendInput` 控制。
- client 可按百分比请求编码缩放，但当前不支持独立指定宽高；显示模式变化后仍需要重启 host。
- video DataChannel 使用无序、不重传策略，拥塞或丢包时会跳过旧帧并请求关键帧；当前没有自适应码率。

## 源码结构

- `src/desktop_capture.*`：DXGI adapter/output 选择与 Desktop Duplication
- `src/main.cpp`：NVENC AV1 配置、采集/编码循环、键鼠注入与重连逻辑
- `src/host_identity.*`：Host 长期设备凭证的 Windows DPAPI 持久化
- `src/webrtc_transport.*`：无信令依赖的 WebRTC DataChannel 传输层
- `src/webrtc_tcp_bootstrap.*`：信令帧编解码与协商状态机（同时供 WSS adapter 和测试复用）
- `src/webrtc_websocket_signaling.*`：libdatachannel WebSocket/WSS 信令适配层
- `src/protocol.h`：host/client 共用的 wire protocol 定义
- `src/client_main.cpp`：client WebRTC 分片重组、协议校验与播放线程
- `src/vpl_decoder.*`：Intel oneVPL AV1 D3D11 硬件解码
- `src/video_window.*`：D3D11 Video Processor、flip-model 窗口呈现与客户端输入采集

第三方 NVIDIA 示例封装源码直接从 SDK 路径参与构建，没有复制或修改 SDK 文件。

`signaling-server/` 是使用 SQLite 保存 passkey、网页登录和 Host 列表的 Node.js 服务，同时只转发
WebRTC SDP/ICE 二进制帧；`deploy/` 包含生产用 systemd、Caddy 和 coturn STUN-only 配置。信令与
STUN 服务不接触 DataChannel 或视频内容。完整服务器配置、更新和排障步骤见
[`docs/signaling-server-deployment.md`](docs/signaling-server-deployment.md)。
