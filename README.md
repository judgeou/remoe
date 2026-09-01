<p align="center">
  <img src="android-client/design/app-icon.svg" width="144" height="144" alt="remoe 图标">
</p>

<h1 align="center">remoe</h1>

<p align="center">
  <strong>面向自托管场景的低延迟、跨平台远程桌面系统</strong>
</p>

<p align="center">
  Windows Host · Web Client · Android Client · Windows Native Client
</p>

## 项目概述

remoe 是一套以 WebRTC 为传输基础的远程桌面系统，面向重视低延迟、硬件加速、移动端交互与
自主管理服务端的使用场景。系统通过 WSS 完成身份认证与连接协商；会话建立后，视频、键鼠控制和
纯文本剪贴板直接通过 WebRTC 传输，信令服务不转发桌面画面或业务数据。

Windows Host 使用 Desktop Duplication API 捕获指定显示器，优先通过 Intel oneVPL 编码低延迟 AV1，
不可用时回退至 NVIDIA NVENC AV1。独立的兼容版本 `remoe_host_x264.exe` 使用 GDI BitBlt、libyuv
和 x264 生成 H.264，适用于 Microsoft Basic Render Driver、Matrox G200e 等不具备可靠硬件采集或
AV1 编码能力的环境。

Web、Android 和 Windows 原生客户端共享同一连接协议。账号以 passkey 登录；Windows Host 通过
短期配对码加入账号，Android 设备则通过二维码批准流程和 Android Keystore 中不可导出的设备密钥
完成绑定。一个账号可以管理多台 Host 和多台客户端设备。

> [!IMPORTANT]
> 当前版本为 `0.1.0`，仍处于积极开发阶段。协议、部署方式和兼容性边界可能在正式稳定版之前调整。
> 当前未实现音频与文件传输，并且只支持 STUN 直连，不提供 TURN 中继。

## 核心能力

- **低延迟视频链路**：使用标准 WebRTC VideoTrack、RTP/SRTP、NACK、PLI 和发送节奏控制，支持
  AV1 与 H.264。
- **硬件加速与功耗控制**：Windows AV1 路径使用 D3D11、oneVPL 或 NVENC；原生 Windows Client
  使用 oneVPL 和 D3D11 视频内存解码、缩放及呈现，避免逐帧回读 CPU。
- **跨平台客户端**：提供 Chromium 浏览器客户端、原生 Android 客户端和 Windows 原生客户端。
- **远程输入与剪贴板**：支持键鼠控制、移动端触控板指针、点击/拖拽、双指滚动、缩放和平移，
  以及双向纯文本剪贴板。
- **账号与设备管理**：支持 passkey、Host 配对、Android 设备密钥、设备列表和一次性连接邀请。
- **自托管服务**：提供 Node.js/SQLite 信令与账号服务，以及 Caddy、systemd 和 STUN-only coturn
  部署配置。

## 系统组成

| 组件 | 位置或产物 | 职责 |
|---|---|---|
| Windows Host | `remoe_host.exe` | DXGI 桌面捕获，Intel/NVIDIA AV1 硬件编码，远程输入执行 |
| 兼容 Host | `remoe_host_x264.exe` | GDI 桌面捕获与 x264 H.264 软件编码 |
| Web Client | `web-client/` | 浏览器登录、设备管理、WebRTC 播放和远程控制 |
| Android Client | `android-client/` | 原生设备绑定、硬件解码、触控板式远程操作 |
| Windows Client | `remoe_client.exe` | oneVPL AV1 硬件解码与 D3D11 低开销呈现 |
| 信令服务 | `signaling-server/` | passkey、设备状态、一次性邀请和 SDP/ICE 协商 |
| 部署配置 | `deploy/` | Caddy、systemd 与 coturn STUN-only 生产配置 |

## 文档导航

- [Android 客户端说明](android-client/README.md)
- [Android 客户端设计与阶段规划](docs/android-client-design.md)
- [信令服务生产部署](docs/signaling-server-deployment.md)
- [WebRTC 技术评估](docs/webrtc-evaluation.md)

## 环境要求

- Windows 10/11 x64
- Visual Studio 2022（Desktop development with C++）及 Windows 10/11 SDK
- CMake 3.24+
- 支持 AV1 硬件编码的 Intel GPU 和当前 Intel 图形驱动；或作为回退，支持 AV1 NVENC 的
  NVIDIA GPU 和兼容驱动（通常为 Ada Lovelace 或更新架构）
- NVIDIA Video Codec SDK 13.0.37，默认路径：
  `third_party/NVENC_Video_Codec_SDK_13.0.37`
- libdatachannel 0.24.5 及其固定版本子依赖，默认路径：
  `third_party/libdatachannel-0.24.5`
- Mbed TLS 3.6.7 源码，默认路径：`third_party/mbedtls-3.6.7`
- 构建 Intel host 编码器或 client 时需要 Intel oneVPL 2.10+ 源码，默认路径：
  `third_party/libvpl`
- 运行 client 时需要支持 AV1 硬件解码的 Intel GPU 和当前 Intel 图形驱动

仅构建 `remoe_host_x264.exe` 时还需要：

- libyuv 源码，默认路径：`third_party/libyuv`
- x264 源码及以 MSVC 构建的静态库，默认分别为 `third_party/x264` 和
  `third_party/x264/build-msvc/libx264.lib`
- NASM，以及能够执行 x264 `configure`/`make` 的 MSYS2、Git Bash 或 WSL 环境

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

当 `third_party/libvpl` 存在时，host 会编入 Intel AV1 编码支持并生成
`build-local/Release/remoe_client.exe`。如果 oneVPL 源码不在默认位置，可在手动配置 CMake 时
传入 `-DVPL_ROOT="D:/path/to/libvpl"`。依赖缺失时 host 仍可构建，但只使用 NVENC。NVENC DLL
在发生回退时才动态加载，因此没有安装 NVIDIA 驱动不会影响 Intel 路径启动。

libdatachannel 使用随项目编译的 Mbed TLS 静态加密后端，因此不要求开发机额外安装 OpenSSL SDK。
WSS 连接会将 Windows 当前用户和本机的 `ROOT` 证书库导出给 Mbed TLS，用于验证服务器证书链及
域名；证书无效时连接会直接失败，不会退化为跳过验证。
`src/webrtc_transport.*` 提供 host/client 共用的 WebRTC 传输封装。可靠有序的 control DataChannel
承载参数协商和键鼠输入；标准 VideoTrack 通过 RTP 承载 H.264 或 AV1 编码帧。SDP/ICE 经 WSS 中继，
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

### 单独构建 GDI + x264 Host

x264 需先在已加载 x64 MSVC 环境的 shell 中配置为静态库。下面的 `bash` 和 `make` 可来自
MSYS2/Git Bash；若使用 WSL，需要把 `CC` 和 `AS` 指向 Windows 的 `cl.exe` 与 `nasm.exe`：

```powershell
New-Item -ItemType Directory -Force third_party\x264\build-msvc | Out-Null
Set-Location third_party\x264\build-msvc
bash ../configure --host=x86_64-w64-mingw32 --enable-static --disable-cli --disable-opencl
make -j
Set-Location ..\..\..

cmake -S . -B build-local -DREMOE_BUILD_X264_HOST=ON
cmake --build build-local --config Release --target remoe_host_x264
.\build-local\Release\remoe_host_x264.exe --check-encoder
```

若依赖不在默认位置，可设置 `LIBYUV_ROOT`、`X264_ROOT`、`X264_BUILD_ROOT` 和 `X264_LIBRARY`。
该功能默认关闭，因而不会改变普通 `remoe_host.exe` 的构建或许可证边界。

`remoe_host_x264.exe` 静态链接 GPL x264，分发该二进制时应把它作为 GPL 版本处理，提供对应完整源码
和 GPL 许可证，并确保项目中与其组合的代码许可证兼容。此目标没有 NVIDIA 功能或 NVIDIA SDK 依赖；
`remoe_host.exe` 仍是原有 Intel/NVIDIA AV1 版本。这里是构建结构说明，不替代针对具体发行方式的法律意见。

### GitHub 自动构建与发布

仓库中的 `.github/workflows/release.yml` 会在向 `main` 推送或提交 Pull Request 时自动执行
Windows x64 Release 构建和原生测试。构建结果可从对应 GitHub Actions 运行记录的 Artifacts 中下载。

推送以 `v` 开头的版本标签时，工作流还会创建 GitHub Release，并上传包含
`remoe_host.exe`、可用时的 `remoe_client.exe` 以及本说明文件的 `remoe-windows-x64.zip`：

```powershell
git tag v0.1.0
git push origin v0.1.0
```

发布新版本前需要同步更新 `CMakeLists.txt` 中 `project(... VERSION ...)` 的版本号。

## 运行

`remoe_host.exe` 默认以当前用户权限运行；首次配置防火墙时会为规则安装弹出一次 UAC。若希望 host
本身也以管理员权限运行，可添加 `--admin`，程序会使用 Windows `runas` 重新启动自身；如果用户拒绝
授权，host 不会运行。管理员权限可以减少
普通高完整性窗口的交互限制，但 Windows UAC 安全桌面仍属于隔离桌面，当前版本不能采集或远程操作
安全桌面本身。

首次正常启动时，host 会检查是否存在精确绑定当前 `remoe_host.exe` 路径的入站 UDP 防火墙规则。
规则缺失、禁用或指向旧路径时会弹出一次 UAC 请求，随后创建适用于 Domain、Private 和 Public
网络的 `remoe host WebRTC UDP` 程序规则。`--help` 和 `--check-encoder` 不触发该检查。

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
| `--check-encoder` | 关闭 | 无需信令，采集并硬件编码一个 AV1 测试帧后退出 |
| `--repair` | 关闭 | 在 Host 本机重新生成配对码、转移账号并轮换设备凭证 |
| `--legacy-invite` | 关闭 | 兼容模式：自动生成并打印匿名邀请 URL |

对 `remoe_host_x264.exe`，`--output` 是 GDI `EnumDisplayMonitors` 的顺序；`--check-encoder` 测试
GDI 抓屏和 x264 H.264 软件编码。它接受同一套连接参数，但码率上限为 50 Mbps。GDI 路径会把鼠标
指针合成到画面中。H.264 和 AV1 都通过标准 WebRTC VideoTrack 发送；浏览器使用原生 WebRTC 解码，
Windows 原生 oneVPL Client 仍只解 AV1。

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

## Windows 原生 Client

直接启动 `remoe_client.exe` 会显示 Windows 登录窗口。输入与网页相同的 HTTPS 服务 origin（例如
`https://signal.example.com`），点击“使用 passkey 登录”后，程序会打开系统默认浏览器。浏览器使用
现有 passkey 账号批准原生 Client；程序通过 HTTPS 轮询一次性设备授权结果，不读取浏览器 Cookie，
也不会接触 passkey 私钥。登录完成后可直接从“我的电脑”列表选择在线 Host，并设置 FPS、码率和缩放。
点击连接会保留启动器，在它前方新建一个最大化播放窗口；关闭播放窗口只断开当前会话，随后可以
回到启动器重新选择 Host。

原生 Client 获得的长期 refresh token 使用当前 Windows 用户的 DPAPI 加密，保存在
`%LOCALAPPDATA%\remoe\client-identity.bin`。短期 access token 只保存在内存；点击“退出登录”会撤销
服务器会话并删除本地凭据。服务器仍会在用户点击连接后生成一次短期 invite，但它只作为内部 WebRTC
bootstrap 参数传递，不再要求用户查看、复制或输入 URL。

### 临时邀请兼容模式

原生 client 暂时继续支持匿名邀请 URL，用于兼容和诊断。Host 需显式添加 `--legacy-invite`，然后把
它打印的 URL 交给 client；两端都不需要 `--host` 或 `--port`：

```powershell
.\build-local\Release\remoe_host.exe `
  --signal-url "wss://signal.example.com/signal" --legacy-invite

.\build-local\Release\remoe_client.exe `
  --signal-url "wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT" `
  --fps 60 --bitrate 20 --scale 75
```

兼容模式需要显式传入 `--signal-url`；正常的窗体登录不需要该参数。client 强制要求 Intel AV1 D3D11
硬件解码。如果没有匹配的 Intel GPU、驱动或 oneVPL runtime，
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

`--fps` 可选范围为 1–240；默认使用 `--bitrate` 指定 CBR 码率，单位 Mbps，可选范围为 1–1000。
也可以通过 `--quality 1–51` 选择固定质量模式，数值越小质量越高，默认测试值为 28；固定质量模式
不设置目标码率，Intel oneVPL 使用 ICQ，NVIDIA NVENC 使用 CONSTQP。
`--scale` 是 host 编码分辨率相对被捕获显示器的百分比，可选范围为 10–100，默认 100。例如源画面
为 2560×1440 时，`--scale 75` 会请求 1920×1080。最终宽高会向下对齐到偶数，并由
`StreamHeader` 回传。缩放在 host 的 D3D11 GPU 视频处理器中完成，不回读 CPU。

## WebRTC 传输协议 v11

仓库中的 `web-client/` 是 Chromium 优先的浏览器客户端。它使用 passkey 账号设备列表、STUN-only ICE、
标准 H.264/AV1 VideoTrack 和 protocol v11；可靠有序的 `remoe-control` DataChannel 发送键鼠
`InputEvent` 与 UTF-8 文本剪贴板。连接后
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
依次承载 `ClientConfig`、`StreamHeader`、`StreamReady`、`InputEvent` 和 `ClipboardHeader + text`。
编码视频由标准 RTP/SRTP VideoTrack 承载。libdatachannel 负责 H.264/AV1 RTP 分片、Sender Report、
NACK 重传缓存和 PLI；浏览器直接消费远端 `MediaStreamTrack`，原生 Client 在 RTP 解包后送入 oneVPL。

host 在 RTP 发送链末端使用有界漏桶 pacer。CBR 从 client 请求值开始，并把它作为上限；发送节奏
默认是工作码率的 1.5 倍、每 2 ms 一批，批量大小随码率和间隔计算。host 根据 RTCP
Receiver Report、NACK、PLI、连接 RTT、发送队列延迟和本机调度迟滞做 AIMD 调节：连续 3 次干净报告
才小幅升码率，丢包或排队则立即降码率，并在必要时请求新关键帧。调度不稳时发送间隔可放宽为
3/5 ms；高码率始终保持 2 ms，避免较长间隔形成大 UDP 突发。队列中最老数据实际等待达到 100 ms
时跳过尚未编码的新帧，极端溢出时整批丢弃，因此不会无限积压旧画面。
NVENC、oneVPL 和 x264 均支持运行时更新 CBR。固定质量模式不改变编码质量，仍使用 1.5 倍、2 ms
的平滑 pacer，但不参与 CBR 自适应；单个大帧即使超过名义队列容量也会完整接纳，发送完成前跳过
后续采集，避免丢帧触发 PLI/IDR 循环。

### ClientConfig（36 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMCF` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `36` |
| 8 | u32 | fps_num | client 请求的帧率分子 |
| 12 | u32 | fps_den | 帧率分母，当前必须为 1 |
| 16 | u32 | bitrate_bps | 网络媒体上限；CBR 时由 host 在该上限内动态选择工作码率 |
| 20 | u32 | scale_percent | client 请求的编码分辨率百分比，10–100 |
| 24 | u32 | flags | bit 0 = 支持双向 UTF-8 文本剪贴板；其他位必须为 0 |
| 28 | u32 | rate_control | 0=CBR；1=固定质量 |
| 32 | u32 | quality | 固定质量为 1–51（小=高质量）；CBR 为 0 |

### StreamHeader（44 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `RMOE` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `44` |
| 8 | u32 | codec | `AV01` 或 `H264` |
| 12 | u32 | width | 编码宽度 |
| 16 | u32 | height | 编码高度 |
| 20 | u32 | fps_num | 帧率分子 |
| 24 | u32 | fps_den | 帧率分母，当前为 1 |
| 28 | u32 | bitrate_bps | 回显 client 请求的网络媒体上限 |
| 32 | u32 | codec_profile | AV1 为 0；H.264 为 `profile_idc << 16 | constraints << 8 | level_idc` |
| 36 | u32 | rate_control | 0=CBR；1=固定质量 |
| 40 | u32 | quality | 固定质量值；CBR 为 0 |

### StreamReady（8 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `SRDY` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `8` |

client 完成解码队列和窗口初始化后发送此消息；host 收到后才开始发送视频。

### ClockSyncRequest / ClockSyncResponse

原生 client 在开始播放前发送多次时钟同步请求，并在播放期间定期重采样。Host 使用与视频帧
`timestamp_us` 相同的单调时钟基准返回接收和发送时间。Client 采用 NTP 四时间戳算法，在最近 8 个
样本的滑动窗口中选择最低 RTT
样本估算时钟偏移；标题栏的 `Age` 表示从 Host 帧时间戳到实际提交显示的估算年龄。该估算不依赖
系统墙上时钟，但上下行路径固有的不对称仍可能带来约为路径时延差一半的误差。

`ClockSyncRequest` 为 24 bytes：`CCLK` magic、version、header size、sequence、reserved，以及
`client_send_us`。`ClockSyncResponse` 为 40 bytes，在回显 sequence 和 `client_send_us` 后追加
`host_receive_us` 与 `host_send_us`。

### 标准 VideoTrack

VideoTrack 的 codec 通过 SDP 协商。H.264 Host 向 RTP packetizer 提交 Annex-B access unit，IDR 携带
SPS/PPS；AV1 Host 提交不带 IVF 容器的 temporal unit。视频包格式遵循对应 RTP payload 规范，不再定义
Remoe 私有的 `VideoChunkHeader`。接收端通过 RTCP PLI 请求关键帧，丢包恢复由 RTCP/NACK 和解码队列
共同处理。

### InputEvent（24 bytes）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `INPT` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `24` |
| 8 | u16 | type | 1=移动；2–6=左/右/中/X1/X2；7/8=垂直/水平滚轮；9=键盘 |
| 10 | u16 | flags | bit 0=释放；bit 1=扩展扫描码 |
| 12 | i32 | value1 | 移动 X（0–65535）、滚轮 delta 或 Windows 扫描码 |
| 16 | i32 | value2 | 移动 Y（0–65535），其余类型为 0 |
| 20 | u32 | sequence | client 递增事件编号 |

### ClipboardHeader（16 bytes + UTF-8 text）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `CLIP` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `16` |
| 8 | u32 | payload_size | 随后的 UTF-8 文本字节数，最大 1 MiB |
| 12 | u32 | sequence | 发送方递增消息编号 |

剪贴板消息可双向发送，只承载纯文本且通过可靠有序的端到端 DataChannel，不经过信令服务器。
Windows 原生 client 会自动同步双方剪贴板。网页收到远程文本后会尝试写入浏览器剪贴板；如果浏览器
要求用户手势，工具栏的“接收剪贴板”会亮起，点击即可完成。工具栏的“发送剪贴板”用于把浏览器
本地剪贴板发送到 Host。

### WebRtcSignalHeader（20 bytes，仅连接初始化阶段）

| 偏移 | 类型 | 字段 | 值/说明 |
|---:|---|---|---|
| 0 | u32 | magic | `WRMS` |
| 4 | u16 | version | `11` |
| 6 | u16 | header_size | `20` |
| 8 | u16 | type | 1=SDP；2=ICE candidate；3=DataChannel ready；4=完成确认 |
| 10 | u16 | reserved | 0 |
| 12 | u32 | value_size | 后续 value 长度 |
| 16 | u32 | metadata_size | value 后的 metadata 长度 |

type 1 的 value/metadata 分别是 SDP 和 `offer`/`answer`；type 2 分别是 candidate 和 `mid`；
type 3/4 不带 payload。双方完成确认后，WSS 不再参与业务数据传输。

鼠标坐标相对实际视频区域归一化，窗口宽高比不同产生的黑边不参与映射；拖动越过视频边缘时坐标
会夹到边缘。host 将坐标映射回被捕获的 DXGI output，因此也支持位于负坐标的副显示器。

client 连接后的第一张图像强制为 IDR/key frame，并携带所需 codec headers，因而 client 无需
连接建立前的码流状态。断线后 host 返回监听状态，支持后续 client 重连；同一时刻只服务一个 client。

## 当前限制与安全边界

- 网页账号使用 WebAuthn/passkey 与浏览器会话 Cookie；credential ID 长期保存在浏览器本地，用于兼容
  只能创建不可发现 WebAuthn 凭据的设备。Host 使用 DPAPI 保护的长期 bearer token。
  该简化模型信任服务器，登录 Cookie 或 Host token 被当前用户上下文中的恶意程序窃取时仍可能被冒用。
- 恢复码是约 130 bit 熵的单次 bearer secret，服务端只保存哈希；使用后会在新 passkey 注册成功的
  同一数据库事务中轮换。全部账号凭据丢失时可在 Host 本机重新配对，损失仅为原账号的 Host 列表。
- 自动配置 STUN，但有意禁用 TURN；对称 NAT、UDP 被封锁等无法直连的网络仍可能连接失败。
- Desktop Duplication API 不会自动把硬件鼠标指针合成到桌面纹理，当前画面可能不显示鼠标指针。
- GDI/x264 路径是 CPU 抓屏、缩放、色彩转换和编码，兼容性优先，性能与帧率取决于服务器 CPU；
  某些 RDP 会话、锁屏状态、受保护内容或显示驱动仍可能返回黑屏或静止画面。
- 锁屏、UAC 安全桌面、显示模式切换和部分受保护内容不能正常捕获。
- `SendInput` 受 Windows UIPI 限制。控制高完整性应用时 host 通常也需要 `--admin`；即使提升权限，
  `Ctrl+Alt+Del` 和 UAC 安全桌面仍不能通过普通 `SendInput` 控制。
- 剪贴板同步当前仅支持最多 1 MiB 的纯文本，不传输图片、文件列表或富文本。网页读写剪贴板还受
  HTTPS、安全上下文、页面焦点和浏览器用户手势策略约束。
- client 可按百分比请求编码缩放，但当前不支持独立指定宽高；显示模式变化后仍需要重启 host。
- 自适应控制目前调整码率、发送间隔和关键帧，但尚不动态调整分辨率或帧率，也不等同于完整的
  WebRTC GCC/TWCC 拥塞控制。

## 源码结构

- `src/desktop_capture.*`：DXGI adapter/output 选择与 Desktop Duplication
- `src/gdi_capture.*`：GDI BitBlt 显示器抓取与鼠标指针合成
- `src/clipboard.*`：Windows UTF-16 剪贴板与 wire UTF-8 文本之间的转换和消息校验
- `src/main.cpp`：采集/编码循环、键鼠注入与重连逻辑
- `src/adaptive_stream_controller.*`：RTCP/RTT/发送队列驱动的 CBR AIMD 决策
- `src/video_encoder.*`：AV1 编码器抽象与 Intel oneVPL → NVIDIA NVENC 选择策略
- `src/vpl_encoder.cpp`：Intel oneVPL D3D11/NV12 AV1 硬件编码
- `src/nvenc_encoder.cpp`、`src/nvenc_api_loader.cpp`：NVENC AV1 回退与驱动 DLL 延迟加载
- `src/x264_encoder.*`：libyuv BGRA 缩放/转 I420 与 x264 Annex-B H.264 软件编码
- `src/host_identity.*`：Host 长期设备凭证的 Windows DPAPI 持久化
- `src/webrtc_transport.*`：无信令依赖的 WebRTC control DataChannel、VideoTrack 与 RTP/RTCP 处理
- `src/webrtc_tcp_bootstrap.*`：信令帧编解码与协商状态机（同时供 WSS adapter 和测试复用）
- `src/webrtc_websocket_signaling.*`：libdatachannel WebSocket/WSS 信令适配层
- `src/protocol.h`：host/client 共用的 wire protocol 定义
- `src/client_main.cpp`：client RTP 视频帧接收、协议校验与播放线程
- `src/vpl_decoder.*`：Intel oneVPL AV1 D3D11 硬件解码
- `src/video_window.*`：D3D11 Video Processor、flip-model 窗口呈现与客户端输入采集

第三方 NVIDIA 示例封装源码直接从 SDK 路径参与构建，没有复制或修改 SDK 文件。

`signaling-server/` 是使用 SQLite 保存 passkey、网页登录和 Host 列表的 Node.js 服务，同时只转发
WebRTC SDP/ICE 二进制帧；`deploy/` 包含生产用 systemd、Caddy 和 coturn STUN-only 配置。信令与
STUN 服务不接触 DataChannel 或视频内容。完整服务器配置、更新和排障步骤见
[`docs/signaling-server-deployment.md`](docs/signaling-server-deployment.md)。

Android 原生客户端的架构、扫码绑定 Passkey、Digital Asset Links、WebRTC、输入、测试与分阶段
实施方案见 [`docs/android-client-design.md`](docs/android-client-design.md)。
