# WebSocket 信令服务器部署

本文说明如何在 Debian/Ubuntu 服务器部署 remoe 的网页验证客户端、WSS 信令中继和 STUN-only 服务。示例域名
统一使用 `signal.example.com`，部署时替换为自己的域名。

信令服务器只转发 SDP/ICE bootstrap 的二进制帧，不承载 DataChannel 业务数据。STUN 由同机 coturn
单独提供，TURN 禁用。Node.js 只监听 `127.0.0.1:8080`，公网入口由 Caddy 提供 HTTPS/WSS。

## 前置条件

- 一台具有公网 IPv4 或 IPv6 的 Debian/Ubuntu 服务器。
- 域名的 A/AAAA 记录已经指向服务器。
- 云安全组和主机防火墙允许 TCP 80、443；SSH 管理端口也需保持可用。
- 服务器上的 Node.js 18 或更高版本，用于信令服务。
- 构建网页的开发机使用 Node.js 22.18 或更高版本。

先安装运行依赖：

```bash
sudo apt update
sudo apt install -y nodejs npm caddy
node --version
```

如果发行版仓库中的 Node.js 低于 18，请先通过可信的软件源安装受支持版本，再继续部署。

## 安装信令服务

以下命令在 remoe 仓库根目录执行：

```bash
# 部署前先运行单元测试
cd signaling-server
npm ci
npm test
cd ..

# 创建不可登录的专用服务账号
if ! id -u remoe-signal >/dev/null 2>&1; then
  sudo useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin remoe-signal
fi

# 安装生产文件。node_modules 由 root 管理，服务账号只有读取权限
sudo install -d -o root -g root -m 0755 /opt/remoe/signaling-server
sudo install -o root -g root -m 0644 \
  signaling-server/package.json \
  signaling-server/package-lock.json \
  signaling-server/server.mjs \
  /opt/remoe/signaling-server/

cd /opt/remoe/signaling-server
sudo npm ci --omit=dev --ignore-scripts
sudo chown -R root:root /opt/remoe/signaling-server
```

安装并启动 systemd 单元：

```bash
cd /path/to/remoe
sudo install -o root -g root -m 0644 \
  deploy/remoe-signaling.service \
  /etc/systemd/system/remoe-signaling.service
sudo systemctl daemon-reload
sudo systemctl enable --now remoe-signaling.service
sudo systemctl status --no-pager remoe-signaling.service
curl --fail http://127.0.0.1:8080/healthz
```

模板默认设置如下：

- `REMOE_SIGNAL_HOST=127.0.0.1`
- `REMOE_SIGNAL_PORT=8080`
- 内存上限 256 MiB
- 异常退出后自动重启
- 通过 systemd 限制文件系统、设备和 Linux capabilities

如需修改监听端口，应同时修改 systemd 单元和 Caddy upstream，但不建议把 Node.js 直接监听到公网。

## 配置 Caddy 和 HTTPS

仓库中的 `deploy/Caddyfile` 从 `REMOE_SIGNAL_DOMAIN` 环境变量读取域名，未设置时使用
`signal.example.com`。为 Caddy 创建 systemd override：

```bash
sudo systemctl edit caddy
```

填入以下内容，将域名替换为真实值：

```ini
[Service]
Environment=REMOE_SIGNAL_DOMAIN=signal.example.com
```

然后安装并验证 Caddyfile：

```bash
cd /path/to/remoe
sudo install -o root -g root -m 0644 deploy/Caddyfile /etc/caddy/Caddyfile
sudo env REMOE_SIGNAL_DOMAIN=signal.example.com \
  caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
sudo systemctl daemon-reload
sudo systemctl restart caddy
sudo systemctl enable caddy
sudo systemctl status --no-pager caddy
```

Caddy 会在 DNS 和公网端口正确时自动申请 TLS 证书，将 `/signal` 与 `/healthz` 反向代理到
`127.0.0.1:8080`，并从 `/opt/remoe/web-client/current` 提供网页客户端。

## 构建和部署网页客户端

网页使用 Vite、Vue 3 和 TypeScript，构建在开发机完成，服务器只接收静态 `dist/`。在仓库根目录
执行：

```bash
cd web-client
npm ci
npm test
npm run build
npm run deploy -- --server root@signal.example.com
```

`npm run build` 会先运行 `vue-tsc` 类型检查，再由 Vite 生成带内容哈希的生产文件。部署脚本检查
`dist/index.html` 与 `dist/assets/`，通过一次 SSH 连接把压缩包上传到
`/opt/remoe/web-client/releases/<release-id>`，最后原子切换 `current` 软链接。因此上传期间不会出现
新 HTML 引用尚未上传完成的资源，也不会重复触发多次 SSH 认证。

也可通过环境变量指定服务器：

```bash
REMOE_WEB_SERVER=root@signal.example.com npm run deploy
```

`--target` 可以修改远端目录，但出于安全限制必须位于 `/opt/remoe/` 内。旧 release 默认保留，出现
问题时可以把 `current` 软链接切回上一版。

## 部署 STUN-only

STUN 使用同一域名的 UDP 3478，但不经过 Caddy。云安全组和主机防火墙必须分别允许 IPv4、IPv6
入站 UDP 3478；不要开放 TURN relay 端口范围。

安装 coturn 并使用仓库提供的严格 STUN-only 配置：

```bash
sudo apt update
sudo apt install -y coturn
cd /path/to/remoe
sudo install -o root -g root -m 0644 \
  deploy/coturn-stun.conf /etc/turnserver.conf
sudo systemctl enable --now coturn
sudo systemctl restart coturn
sudo systemctl status --no-pager coturn
sudo ss -lunp | grep ':3478'
```

`deploy/coturn-stun.conf` 只启用 UDP STUN Binding，配置了 `stun-only`，并同时关闭 TCP、TLS 和
DTLS listener。`stun-only` 会拒绝所有 TURN Allocate，因此该服务不需要 TURN 用户名、密码、
证书或 relay 端口范围。

更新配置后重启服务并检查日志：

```bash
sudo systemctl restart coturn
sudo journalctl -u coturn -n 100 --no-pager
```

## 验证部署

公网健康检查应返回 HTTP 200 和 JSON：

```bash
curl --fail https://signal.example.com/healthz
curl --fail https://signal.example.com/ | grep 'remoe 网页客户端'
```

预期格式：

```json
{"status":"ok","sessions":0}
```

在 Windows host 上启动：

```powershell
.\build-local\Release\remoe_host.exe `
  --signal-url "wss://signal.example.com/signal"
```

信令服务器确认 Host session 注册成功后，host 才会打印包含 21 字符 Nano ID 的邀请 URL。client
使用完整邀请 URL：

```powershell
.\build-local\Release\remoe_client.exe `
  --signal-url "wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT"
```

client 不需要 host 地址或端口；视频和键鼠均由端到端 WebRTC DataChannel 承载，服务器只看见
SDP/ICE 信令。应用会从 WSS URL 自动派生同域名的 `stun:signal.example.com:3478`，无需增加 STUN
命令行参数；成功生成服务器反射地址后会打印 `WebRTC STUN reflexive candidate gathered`。
Host 会保持这一条 WSS 连接等待 Client，不会每 15 秒重连。Windows 程序从系统 `ROOT` 证书库
向 Mbed TLS 提供 CA bundle，因此 WSS 会验证证书链和域名，无效证书会被拒绝。
服务器不会由 Client 创建 session；错误或已过期的邀请会立即返回 `Invite not found or expired`。

### 使用网页验证客户端

先启动 Host 并复制它打印的完整 WSS invite。使用最新版 Chrome 或 Edge 打开：

```text
https://signal.example.com/
```

把 WSS invite 粘贴到输入框，点击“连接并验证”。也可以把 invite 的 `#` 及其后内容附加到网页
地址，例如 `https://signal.example.com/#V1StGXR8_Z5jdHi6B-myT`；URL fragment 不会随 HTTP 请求发送到
服务器。页面成功建立两条 DataChannel、重组 NVENC AV1 并由 WebCodecs 显示第一帧后，会让画面
自动占满网页。点击画面中央的按钮即可通过 Pointer Lock 接管键鼠，按 `Esc` 释放；右上角按钮用于
断开。页面会按视频与浏览器视口的实际尺寸等比缩放，保证整张远程画面可见，并绘制一个与发送到
Host 的绝对坐标同步的本地鼠标指针。浏览器要求 Pointer Lock 由一次明确的用户操作触发，因此它
不能在异步连接完成时自动启用。

若页面报告浏览器不支持 AV1 配置，先在 `chrome://gpu` 或 `edge://gpu` 检查硬件视频解码情况。
WebCodecs 的 codec 支持由浏览器和机器共同决定，页面会在发送 `StreamReady` 前调用
`VideoDecoder.isConfigSupported()`，不会在已知不支持时启动 Host 视频发送。

## 更新服务

拉取新代码并完成测试后，只需更新生产文件和依赖：

```bash
cd /path/to/remoe/signaling-server
npm ci
npm test

sudo install -o root -g root -m 0644 \
  package.json package-lock.json server.mjs \
  /opt/remoe/signaling-server/
cd /opt/remoe/signaling-server
sudo npm ci --omit=dev --ignore-scripts
sudo chown -R root:root /opt/remoe/signaling-server
sudo systemctl restart remoe-signaling.service
curl --fail http://127.0.0.1:8080/healthz
```

网页更新在开发机执行，不需要重启 Caddy：

```bash
cd /path/to/remoe/web-client
npm ci
npm test
npm run build
npm run deploy -- --server root@signal.example.com
```

如果 `deploy/Caddyfile` 或 systemd 单元有变化，应重新安装对应文件、运行配置验证，然后再 reload
或 restart 服务。

## 日志和排障

```bash
sudo systemctl status --no-pager --full remoe-signaling.service caddy
sudo systemctl status --no-pager --full coturn
sudo journalctl -u remoe-signaling.service -n 100 --no-pager
sudo journalctl -u caddy -n 100 --no-pager
sudo journalctl -u coturn -n 100 --no-pager
sudo ss -lntp
sudo ss -lunp
```

正常监听关系应为：

- Node.js：`127.0.0.1:8080`
- Caddy：公网 TCP 80、443
- coturn STUN-only：公网 UDP 3478
- SSH：服务器配置的管理端口

常见问题：

- HTTPS 证书申请失败：检查 A/AAAA 记录、TCP 80/443、安全组和主机防火墙。
- `/healthz` 返回 502：检查 `remoe-signaling.service` 是否正在运行，以及 Node.js 是否监听 8080。
- STUN 超时：检查 UDP 3478 的 IPv4/IPv6 云安全组、主机防火墙和 coturn 监听状态。
- 注册响应错误：检查邀请 URL 是否完整、Host 是否仍在线，以及该邀请是否已有 Client 使用。
- DataChannel 超时：检查双方日志是否生成 `srflx` candidate；应用会从 WSS URL 自动派生同域名
  UDP 3478 STUN 地址。禁用 TURN 时跨部分 NAT 仍可能无法直连。

## 安全边界

- 邀请 URL 当前是 bearer secret，不等同于用户身份认证，不应公开传播。
- 信令使用 WSS，Caddyfile 默认添加 HSTS、`nosniff` 和 Referrer Policy。
- 只有已连接的 Host 可以创建 session；Host 断开时 session 立即失效并关闭对应 Client。
- 服务限制单 session 和全局排队内存、会话数量及空闲时间，并清除残留信令帧。
- 不应开放 Node.js 的 8080 端口到公网。
- WebAuthn/passkey 接入后，应由认证流程签发短期授权，而不是仅依赖邀请 URL。
