# WebSocket 信令服务器部署

本文说明如何在 Debian/Ubuntu 服务器部署 remoe 的网页验证客户端、WSS 信令中继和 STUN-only 服务。示例域名
统一使用 `signal.example.com`，部署时替换为自己的域名。

服务使用 SQLite 保存 passkey 公钥、网页登录、轮换恢复码哈希和 Host 列表；WebRTC 部分仍只
转发 SDP/ICE bootstrap 的二进制帧，不承载 DataChannel 业务数据。STUN 由同机 coturn 单独提供，
TURN 禁用。Node.js 只监听 `127.0.0.1:8080`，公网入口由 Caddy 提供 HTTPS/WSS。

## 前置条件

- 一台具有公网 IPv4 或 IPv6 的 Debian/Ubuntu 服务器。
- 域名的 A/AAAA 记录已经指向服务器。
- 云安全组和主机防火墙允许 TCP 80、443；SSH 管理端口也需保持可用。
- 服务器上的 Node.js 22 或更高版本，用于信令和 WebAuthn 服务。
- 构建网页的开发机使用 Node.js 22.18 或更高版本。

先安装运行依赖：

```bash
sudo apt update
sudo apt install -y nodejs npm caddy build-essential python3
node --version
```

如果发行版仓库中的 Node.js 低于 22，可以配置 NodeSource 的签名 APT 仓库：

```bash
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key |
  sudo gpg --dearmor --yes -o /etc/apt/keyrings/nodesource.gpg
echo 'deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_22.x nodistro main' |
  sudo tee /etc/apt/sources.list.d/nodesource.list >/dev/null
sudo apt update
sudo apt install -y nodejs
node --version
```

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

# 安装生产文件。每次更新创建 release，再原子切换 current 软链接
release_id="$(date -u +%Y%m%d%H%M%S)"
release_dir="/opt/remoe/signaling-server/releases/$release_id"
sudo install -d -o root -g root -m 0755 "$release_dir/lib"
sudo install -o root -g root -m 0644 \
  signaling-server/package.json \
  signaling-server/package-lock.json \
  signaling-server/server.mjs \
  "$release_dir/"
sudo install -o root -g root -m 0644 signaling-server/lib/*.mjs \
  "$release_dir/lib/"

cd "$release_dir"
sudo npm ci --omit=dev
sudo chown -R root:root "$release_dir"
sudo ln -sfn "releases/$release_id" /opt/remoe/signaling-server/current.next
sudo mv -Tf /opt/remoe/signaling-server/current.next /opt/remoe/signaling-server/current
```

安装并启动 systemd 单元：

```bash
cd /path/to/remoe
sudo install -o root -g root -m 0644 \
  deploy/remoe-signaling.service \
  /etc/systemd/system/remoe-signaling.service
sudo systemctl daemon-reload

# WebAuthn 凭据与 RP 域名永久绑定；这里必须填写公开 HTTPS 网页的稳定域名
sudo systemctl edit remoe-signaling.service
```

在 override 中填入：

```ini
[Service]
Environment=REMOE_RP_ID=signal.example.com
Environment=REMOE_ORIGIN=https://signal.example.com
```

然后启动：

```bash
sudo systemctl enable --now remoe-signaling.service
sudo systemctl status --no-pager remoe-signaling.service
curl --fail http://127.0.0.1:8080/healthz
```

模板默认设置如下：

- `REMOE_SIGNAL_HOST=127.0.0.1`
- `REMOE_SIGNAL_PORT=8080`
- `REMOE_DATABASE_PATH=/var/lib/remoe/remoe.db`
- `StateDirectory=remoe`，目录权限为 `0700`
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

Caddy 会在 DNS 和公网端口正确时自动申请 TLS 证书，将 `/api/*`、`/host`、`/signal` 与
`/healthz` 反向代理到 `127.0.0.1:8080`，并从 `/opt/remoe/web-client/current` 提供网页客户端。

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
{"status":"ok","sessions":0,"managedHosts":0}
```

在 Windows host 上启动：

```powershell
.\build-local\Release\remoe_host.exe `
  --signal-url "wss://signal.example.com/signal"
```

Host 首次启动会打印类似 `ABCD-EFGH` 的十分钟配对码。打开网页，创建 passkey 账号并保存页面只
显示一次的恢复码，然后输入 Host 配对码和设备名称。配对成功后 Host 会打印：

```text
Host paired and credential protected with Windows DPAPI.
Host is online in the account device list
```

浏览器设备列表中点击“连接”即可；服务器内部生成短期 Nano ID 并把 Browser 分配给在线 Host，用户
看不到 session 或 Host IP。视频和键鼠均由端到端 WebRTC DataChannel 承载，服务器只看见 SDP/ICE
信令。应用会从 WSS URL 自动派生同域名的 `stun:signal.example.com:3478`；成功生成服务器反射地址后
会打印 `WebRTC STUN reflexive candidate gathered`。Host 会保持 WSS 等待 Browser。Windows 程序从系统 `ROOT` 证书库
向 Mbed TLS 提供 CA bundle，因此 WSS 会验证证书链和域名，无效证书会被拒绝。

Host 凭证保存在 `%LOCALAPPDATA%\remoe\host-identity.bin` 并由当前 Windows 用户的 DPAPI 保护。
转移账号或凭证损坏时，在 Host 本机运行相同命令并添加 `--repair`，再把新配对码填入目标账号。旧
设备 token 会轮换，旧账号随即失去该 Host。

### 使用网页验证客户端

使用最新版 Chrome 或 Edge 打开：

```text
https://signal.example.com/
```

通过 passkey 登录，选择在线 Host 并点击“连接”。页面成功建立两条 DataChannel、重组 NVENC AV1
并由 WebCodecs 显示第一帧后，会让画面
自动占满网页。点击画面中央的按钮即可通过 Pointer Lock 接管键鼠，按 `Esc` 释放；右上角按钮用于
断开。页面会按视频与浏览器视口的实际尺寸等比缩放，保证整张远程画面可见，并绘制一个与发送到
Host 的绝对坐标同步的本地鼠标指针。浏览器要求 Pointer Lock 由一次明确的用户操作触发，因此它
不能在异步连接完成时自动启用。

若页面报告浏览器不支持 AV1 配置，先在 `chrome://gpu` 或 `edge://gpu` 检查硬件视频解码情况。
WebCodecs 的 codec 支持由浏览器和机器共同决定，页面会在发送 `StreamReady` 前调用
`VideoDecoder.isConfigSupported()`，不会在已知不支持时启动 Host 视频发送。

“使用临时邀请 URL”折叠入口和原生 client 只用于兼容诊断。需要时给 Host 增加
`--legacy-invite`，再使用它打印的完整 URL；账号管理模式默认不打印邀请。

## 更新服务

首次完成 systemd、环境变量和 Caddy 配置后，后续更新可以在开发机的仓库中一条命令完成。脚本会先
运行测试，然后上传生产文件、安装生产依赖、原子切换 release、重启服务并检查健康状态；如果新版本
无法启动或健康检查失败，会自动恢复上一 release：

```bash
cd /path/to/remoe/signaling-server
npm run deploy -- root@signal.example.com
```

也可以使用显式参数或环境变量：

```bash
npm run deploy -- --server root@signal.example.com
REMOE_SIGNAL_SERVER=root@signal.example.com npm run deploy
```

部署账号需要能够写入 `/opt/remoe/signaling-server`、运行 `npm ci`、切换软链接并重启
`remoe-signaling.service`；仓库默认流程使用 `root`。数据库位于 `/var/lib/remoe/remoe.db`，不会包含在
release 上传和回滚中。

如需手动更新，执行：

```bash
cd /path/to/remoe/signaling-server
npm ci
npm test

release_id="$(date -u +%Y%m%d%H%M%S)"
release_dir="/opt/remoe/signaling-server/releases/$release_id"
sudo install -d -o root -g root -m 0755 "$release_dir/lib"
sudo install -o root -g root -m 0644 package.json package-lock.json server.mjs "$release_dir/"
sudo install -o root -g root -m 0644 lib/*.mjs "$release_dir/lib/"
cd "$release_dir"
sudo npm ci --omit=dev
sudo chown -R root:root "$release_dir"
sudo ln -sfn "releases/$release_id" /opt/remoe/signaling-server/current.next
sudo mv -Tf /opt/remoe/signaling-server/current.next /opt/remoe/signaling-server/current
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
- passkey 无法创建：检查网页是否为 HTTPS、`REMOE_RP_ID` 是否等于部署域名，以及
  `REMOE_ORIGIN` 是否为包含 `https://` 的精确 origin。部分 Android Credential Manager 实现会在
  请求可发现凭据时返回 `NotReadableError`；网页会自动回退到不可发现的本机凭据，并长期保存公开的
  credential ID。清除站点数据或换设备后，使用最新恢复码在该设备重新注册即可。
- 配对码错误：配对码十分钟失效；重新启动未配对 Host，或在已配对 Host 本机使用 `--repair`。
- Host 显示离线：检查 Host 的 WSS 日志以及 `/host` 是否由 Caddy 反向代理。
- DataChannel 超时：检查双方日志是否生成 `srflx` candidate；应用会从 WSS URL 自动派生同域名
  UDP 3478 STUN 地址。禁用 TURN 时跨部分 NAT 仍可能无法直连。

## 安全边界

- 日常网页访问使用 WebAuthn/passkey；登录 Cookie 是只在当前浏览器会话有效的 HttpOnly、
  SameSite=Strict、Secure Cookie。credential ID 不是秘密，会长期保存在浏览器 localStorage，供
  Android 等只能创建不可发现 WebAuthn 凭据的环境在登录时填写 `allowCredentials`。
- 恢复码约有 130 bit 随机熵，服务器只保存 SHA-256 哈希；新 passkey 验证成功后在 SQLite 事务中
  消费旧码并生成新码。恢复码仍是 bearer secret，不应发送给其他人。
- Host 长期 token 由 Windows DPAPI 保护，服务器只保存哈希。该简化模型信任服务器，未使用 TPM
  不可导出密钥或每次连接二次验证。
- 只有账号所有者能给已在线 Host 创建短期 session；Host 断开时 session 立即失效。
- 服务限制单 session 和全局排队内存、会话数量及空闲时间，并清除残留信令帧。
- 不应开放 Node.js 的 8080 端口到公网。
- SQLite 位于 `/var/lib/remoe/remoe.db`；应使用 SQLite backup API 或 `VACUUM INTO` 备份，不要只复制
  正在写入的 WAL 数据库主文件。数据库完全丢失时仍可新建账号并在 Host 本机重新配对。
