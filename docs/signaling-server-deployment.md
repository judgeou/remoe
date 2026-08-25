# WebSocket 信令服务器部署

本文说明如何在 Debian/Ubuntu 服务器部署 remoe 的 WSS 信令中继和 STUN-only 服务。示例域名
统一使用 `signal.example.com`，部署时替换为自己的域名。

信令服务器只转发 SDP/ICE bootstrap 的二进制帧，不承载视频或 WebRTC DataChannel 数据，
也不提供 STUN/TURN。Node.js 只监听 `127.0.0.1:8080`，公网入口由 Caddy 提供 HTTPS/WSS。

## 前置条件

- 一台具有公网 IPv4 或 IPv6 的 Debian/Ubuntu 服务器。
- 域名的 A/AAAA 记录已经指向服务器。
- 云安全组和主机防火墙允许 TCP 80、443；SSH 管理端口也需保持可用。
- Node.js 18 或更高版本。

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

Caddy 会在 DNS 和公网端口正确时自动申请 TLS 证书，并将 `/signal` 与 `/healthz` 反向代理到
`127.0.0.1:8080`。

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

host 会打印包含 21 字符 Nano ID 的邀请 URL。client 使用完整邀请 URL：

```powershell
.\build-local\Release\remoe_client.exe --host <host-address> `
  --signal-url "wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT"
```

目前视频仍通过 host 的 TCP 47990 端口传输，因此 client 仍需要 `--host`，并且该端口必须能从
client 到达。WSS 只替代 SDP/ICE 信令通道，键鼠控制通过 WebRTC DataChannel 传输。应用会从
WSS URL 自动派生同域名的 `stun:signal.example.com:3478`，无需增加 STUN 命令行参数；成功生成
服务器反射地址后会打印 `WebRTC STUN reflexive candidate gathered`。

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
- WebSocket 返回 400：邀请 URL 的 Nano ID 或 role 参数无效。
- WebSocket 返回 409：同一 session 已有相同角色连接；关闭旧进程后重试。
- DataChannel 超时：检查双方日志是否生成 `srflx` candidate；应用会从 WSS URL 自动派生同域名
  UDP 3478 STUN 地址。禁用 TURN 时跨部分 NAT 仍可能无法直连。

## 安全边界

- 邀请 URL 当前是 bearer secret，不等同于用户身份认证，不应公开传播。
- 信令使用 WSS，Caddyfile 默认添加 HSTS、`nosniff` 和 Referrer Policy。
- 服务限制单 session 和全局排队内存、会话数量及空闲时间，并在发送方断开时清除残留帧。
- 不应开放 Node.js 的 8080 端口到公网。
- WebAuthn/passkey 接入后，应由认证流程签发短期授权，而不是仅依赖邀请 URL。
