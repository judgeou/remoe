# remoe 引入 WebRTC 的工程评估

更新时间：2026-08-21

## 1. 结论

ZeroTier 不纳入 remoe 的后续方案。

WebRTC 适合以下目标：

- 在公网环境下自动完成 NAT 穿透；
- 直连失败时通过 TURN 中继；
- 根据带宽、丢包和时延动态调整画质；
- 使用加密的视频和控制通道；
- 为未来的浏览器客户端保留兼容路径。

但对 remoe 来说，引入 WebRTC 并不是简单地把 TCP 替换为 UDP。它会重做传输层、会话建立、丢包恢复和码率控制，同时保留现有 DXGI、NVENC、oneVPL、D3D11 渲染及输入注入模块。

建议采用以下决策边界：

- 如果目标包括公网连接、复杂 NAT、自动中继或浏览器客户端，使用完整的 Google libwebrtc；
- 如果长期仅支持 Windows 原生端的一对一串流，且更关注实现规模和游戏延迟，MsQuic/QUIC 的工程成本更低；
- 不自行实现 ICE、DTLS、SRTP、RTP 拥塞控制等协议；
- 不一次性删除当前 TCP 实现，应先并行增加 WebRTC，再逐步迁移并保留回退选项。

## 2. 推荐架构

```text
Host
 DXGI Desktop Duplication
           |
           | D3D11 Texture
           v
 自定义 WebRTC VideoSource
           |
           v
 自定义 NVENC AV1 Encoder
           |
           | AV1 over RTP/SRTP
           v
      ICE UDP 直连
           |
           +---- 直连失败 ----> TURN UDP/TCP/TLS
                                      |
                                      v
                          自定义 oneVPL AV1 Decoder
                                      |
                                      | D3D11 Texture
                                      v
                                现有 VideoWindow

Client -- WebRTC DataChannel --> Host Input Injector

Client/Host -- HTTPS/WebSocket --> 自建信令服务
```

WebRTC 不定义应用层信令协议。remoe 仍需使用 HTTPS/WebSocket 等方式交换 SDP、ICE candidate、设备身份和会话状态。

参考：[WebRTC Peer Connections](https://webrtc.org/getting-started/peer-connections)

## 3. 可以保留的现有模块

以下模块可以继续使用：

- Host 的 DXGI Desktop Duplication；
- Host 的 `NvEncoderD3D11` AV1 编码；
- GPU 分辨率缩放；
- Client 的 Intel oneVPL AV1 硬件解码；
- D3D11 Video Processor 和窗口显示；
- 当前鼠标、键盘事件结构及 Windows 输入注入逻辑；
- 当前管理员权限和 UIPI 相关处理。

以下模块需要替换或重新适配：

- TCP 监听、连接和帧封包协议；
- 视频发送、接收线程和客户端 AV1 队列；
- 自定义关键帧请求协议；
- 一部分动态码率控制逻辑；
- 连接建立、断线、重连和身份认证流程。

## 4. Host 编码侧改造

需要实现 `NvencAv1EncoderFactory` 和 `NvencAv1Encoder`，接入 WebRTC 的 `VideoEncoder` 接口。

编码适配层需要：

1. 声明支持的 AV1 SDP 格式和 profile；
2. 接收 D3D11 原生纹理，避免转成 I420；
3. 调用现有 NVENC 编码器；
4. 去除当前 IVF 外壳，将 AV1 数据交给 WebRTC RTP packetizer；
5. 为编码结果提供 RTP timestamp、捕获时间、尺寸、帧类型和可用的 QP；
6. 响应 WebRTC 的关键帧请求，强制 NVENC 生成 IDR/关键帧；
7. 响应 `SetRates()`，动态更新目标码率和帧率；
8. 在确有必要时降低编码分辨率，并与现有分辨率百分比能力衔接。

WebRTC 已提供 AV1 RTP packetizer/depacketizer，不需要 remoe 自行实现 AV1 分片。不过，仍需验证 NVENC 输出的 OBU 排列与 packetizer 的要求完全兼容。

参考：

- [WebRTC AV1 RTP packetizer](https://webrtc.googlesource.com/src/+/981d111a875397315b34fa6889809c8862feb60d/modules/rtp_rtcp/source/rtp_packetizer_av1.h)
- [AOMedia AV1 RTP Payload Specification](https://aomediacodec.github.io/av1-rtp-spec/)

需要特别防止 WebRTC 因无法识别自定义原生纹理而触发 D3D11 到 CPU I420 的隐式转换。否则功耗、显存带宽和延迟都会明显上升。

## 5. Client 解码侧改造

需要实现 `VplAv1DecoderFactory`、oneVPL `VideoDecoder` 适配层，以及承载 D3D11 texture 的原生 `VideoFrameBuffer`。

最关键的问题是 oneVPL surface 生命周期。

当前解码流程在回调结束后会很快释放 `mfxFrameSurface1`。WebRTC 的解码帧可能被异步排队、统计和渲染，因此不能在解码回调返回时直接释放。新实现必须：

- 为 oneVPL surface 持有额外引用；
- 将 D3D11 texture 包装为 WebRTC 原生 `VideoFrameBuffer`；
- 等最终渲染完成且 WebRTC 不再引用时释放 surface；
- 正确处理 decoder flush、reset、设备丢失和关键帧恢复；
- 保证正常路径不发生 GPU 到 CPU 再回到 GPU 的拷贝。

WebRTC 的 `VideoFrameBuffer` 支持 `kNative` 缓冲区，因此结构上可以实现 D3D11 零拷贝。

参考：[WebRTC VideoFrame API](https://webrtc.googlesource.com/src/+/refs/heads/main/api/video/video_frame.h)

## 6. 键盘和鼠标控制

建议将控制拆成两个 DataChannel：

| 通道 | 传输模式 | 内容 |
| --- | --- | --- |
| `input-reliable` | 有序、可靠 | 键盘、鼠标按键、滚轮、配置命令 |
| `mouse-motion` | 无序、不重传 | 高频鼠标移动、FPS 相对位移 |

高频移动包丢失后无需重传旧位置，也不应阻塞后续移动事件。鼠标按下事件应携带当时的坐标，避免移动包丢失后在旧位置点击。

通道或 PeerConnection 断开时，Host 必须释放所有远程按键和鼠标按键，防止卡键。FPS 游戏所需的相对鼠标、光标捕获和 raw input 仍属于独立的客户端交互工作。

参考：[W3C WebRTC Specification](https://www.w3.org/TR/webrtc/)

## 7. 信令、STUN 和 TURN

### 7.1 信令服务

信令服务至少需要提供：

- Host 设备注册和在线状态；
- Client 身份认证；
- 设备配对、授权和撤销；
- SDP offer/answer 转发；
- Trickle ICE candidate 转发；
- 会话超时、断开和重连；
- 临时 TURN 凭据签发；
- 访问令牌轮换、限速和审计。

### 7.2 STUN 与 TURN

STUN 用于发现可用的公网 candidate。直连失败时，必须由 TURN 中继，否则无法保证复杂 NAT 和受限网络中的可用性。

建议使用 coturn，并启用：

- TURN UDP；
- TURN TCP；
- TURN TLS；
- 基于 HMAC 的短期凭据；
- 会话数、带宽和用户限额；
- 指标、日志和滥用监控。

参考：[coturn](https://github.com/coturn/coturn)

25 Mbps 视频每小时约产生 11.25 GB 单向媒体数据。经过 TURN 时，中继服务器同时接收和发送，网卡处理的媒体流量约为该数字的两倍，云服务还可能按公网出站流量收费。因此应优先直连，并仅将 TURN 作为兜底。

### 7.3 安全

WebRTC 会加密媒体和 DataChannel，但应用仍需保护信令和设备身份。至少需要：

- TLS 信令连接；
- 设备配对或明确授权；
- 将会话绑定到已认证设备；
- 短期 TURN 凭据；
- 可撤销的访问令牌；
- 防止重放、暴力尝试和 TURN 滥用的限速策略。

WebRTC 不会解决 Windows UAC secure desktop 或 UIPI 权限隔离。Host 是否以管理员权限运行仍会影响输入注入能力。

## 8. 构建系统影响

Google 原生 libwebrtc 使用 Chromium 的 depot_tools、GN 和 Ninja。它不是一个常规的 CMake 子项目，Visual Studio 工程也不是官方主构建方式。

建议：

- 固定一个验证过的 WebRTC commit，不直接跟随 `main`；
- 单独使用 GN/Ninja 构建 libwebrtc；
- 在 remoe CMake 中将产物作为 imported library 引入；
- 对齐 MSVC、Windows SDK、C++ ABI 和 `/MD`/`/MT`；
- 为 Debug 和 Release 分别管理兼容产物；
- 在 CI 中缓存 WebRTC 构建结果；
- 打包 WebRTC 和第三方依赖的许可证文件。

参考：[WebRTC Native Code Development](https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/development/README.md)

libwebrtc 的原生 C++ API 存在变动，部分编码器工厂接口明确处于演进状态。因此必须通过适配层隔离 remoe 业务代码，升级版本时也需要重新验证。

参考：[WebRTC VideoEncoderFactory](https://webrtc.googlesource.com/src/+/8b8e0230e7f05ce18db90febde610c5f1bb1b03f/api/video_codecs/video_encoder_factory.h)

## 9. 推荐实施阶段

### 阶段 0：技术验证

预估：1～2 周。

只验证以下关键链路：

- Windows 可以稳定构建并链接固定版 libwebrtc；
- 两个原生进程可以建立 PeerConnection；
- AV1 SDP 协商成功；
- NVENC AV1 数据可以通过 WebRTC RTP 发送；
- oneVPL 可以解码收到的 AV1；
- D3D11 native buffer 路径没有 CPU 拷贝。

如果零拷贝或 NVENC AV1 packetizer 兼容性无法解决，应在此阶段停止并重新选型，不先开发完整后台。

### 阶段 1：连接和控制

预估：1～2 周。

- 实现简单的 WebSocket 信令；
- 加入 ICE、STUN 和 TURN；
- 将键鼠迁移到 DataChannel；
- 暂时保留当前 TCP 视频，以便分别定位控制和视频问题。

> 实施状态（协议 v11）：工程最终采用 libdatachannel，并已完成 WSS 信令、STUN-only、键鼠 control
> DataChannel，以及标准 H.264/AV1 VideoTrack。Android/Windows 使用 RTP、RTCP NACK、Sender Report
> 和 PLI；Web 端因 Chromium VideoTrack 的播放缓冲改用无序零重传 DataChannel + WebCodecs。
> 运行时已移除 TCP 视频及 host IP/端口参数。标准 track 已加入基于 RR/NACK、RTT 和本地发送队列的
> 有界 pacer 与 AIMD 码率控制；完整 GCC/TWCC 拥塞控制仍属于后续工作。

### 阶段 2：WebRTC 视频

预估：2～4 周。

- 已完成 NVENC/oneVPL 编解码适配和 D3D11 surface 管理；
- 已接入标准 H.264/AV1 RTP packetizer、PLI、NACK 和关键帧恢复；
- 已接入基于网络反馈的自适应 CBR；后续可升级为 GCC/TWCC 并联动帧率和分辨率。

### 阶段 3：公网产品化

预估：3～6 周。

- 设备配对和认证；
- TURN 短期凭据；
- 自动重连和网络切换；
- 码率、帧率和分辨率自适应；
- 指标、日志和故障诊断；
- NAT、防火墙、丢包和高 RTT 测试。

## 10. 工作量估算

以下按一名熟悉 Windows C++、D3D11 和现有代码的开发者估算：

| 范围 | 时间 |
| --- | ---: |
| 本机/局域网 WebRTC 技术原型 | 2～3 周 |
| 可代替当前 TCP 的原生端 MVP | 5～8 周 |
| 公网可用，包含信令、TURN 和认证 | 10～16 周 |
| 增加成熟浏览器客户端 | 额外 3～6 周 |

主要不确定性来自 oneVPL surface 生命周期、libwebrtc 版本适配、NVENC 动态重配置及实际网络下的低延迟调优。

## 11. 风险排序

### 高风险

- libwebrtc 构建、CMake 集成和 API 变动；
- oneVPL/D3D11 解码 surface 的异步生命周期；
- NVENC 与 WebRTC 拥塞控制之间的动态码率和关键帧协调；
- TURN 带宽费用、部署安全和滥用防护。

### 中风险

- NVENC AV1 OBU 与 WebRTC packetizer 的兼容性；
- WebRTC 默认视频策略与桌面/游戏低延迟目标之间的差异；
- 丢包恢复、抖动缓冲和分辨率切换产生的瞬时卡顿；
- DataChannel 的顺序、可靠性配置和断线后的输入状态清理。

### 较低风险

- 现有输入事件结构迁移；
- 复用现有 D3D11 渲染器；
- 初期只协商单一 AV1 codec/profile。

## 12. 验收和测试矩阵

至少覆盖：

- Windows 10/11；
- 多个 NVIDIA 和 Intel 驱动版本；
- 1080p、1440p、4K；
- 30、60、120 FPS；
- 5、25、100 Mbps；
- 5、50、150 ms RTT；
- 0%、0.5%、2%、5% 丢包；
- 带宽突降和恢复；
- ICE UDP 直连；
- TURN UDP；
- TURN TCP/TLS；
- 浏览器视频、动画、托盘菜单和全屏游戏；
- 显示模式切换、睡眠唤醒、显卡设备重置；
- 断线重连、卡键清理和管理员权限场景。

建议记录以下指标：

- selected candidate pair 和连接类型；
- RTT、可用发送码率和实际发送码率；
- packet loss、NACK、PLI；
- jitter buffer delay；
- 编码、发送、接收、解码和渲染帧数；
- dropped frames 和 freeze duration；
- 编码/解码耗时和 QP；
- Host、Client 的 CPU、GPU 和功耗。

## 13. libdatachannel 备选路线

[libdatachannel](https://github.com/paullouisageneau/libdatachannel) 是较轻量的中间方案。

优点：

- 原生 CMake 集成更容易；
- 支持 ICE、STUN、TURN、DTLS、SRTP 和 DataChannel；
- 支持 AV1 媒体传输；
- 构建规模明显小于 Google libwebrtc。

缺点：

- 没有 Google libwebrtc 同等完整的视频拥塞控制、抖动缓冲和质量自适应体系；
- remoe 需要继续维护更多自定义队列、丢包恢复和码率决策；
- 若未来以浏览器互操作和自动视频适应为重点，完整 libwebrtc 更合适。

如果需求只限定为公网穿透、加密 UDP 和控制通道，可以优先验证 libdatachannel；如果希望 WebRTC 真正接管视频自适应并为浏览器客户端铺路，则选择 Google libwebrtc。

## 14. 最终建议

先进行一个最长两周的完整 libwebrtc 技术验证，范围仅限：

```text
DXGI D3D11 Texture
  -> NVENC AV1
  -> WebRTC RTP/SRTP
  -> oneVPL AV1
  -> D3D11 zero-copy render
```

只有该链路验证成功，才继续投入信令、TURN、认证和公网部署。迁移期间保留现有 TCP 模式作为故障回退，待 WebRTC 在局域网、恶劣网络和 TURN 中继场景均通过测试后再考虑移除旧传输层。
