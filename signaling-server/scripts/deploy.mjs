import { randomBytes } from 'node:crypto';
import { access, readdir } from 'node:fs/promises';
import { constants } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

function positionalServer() {
  const args = process.argv.slice(2);
  for (let index = 0; index < args.length; index += 1) {
    if (args[index] === '--server') {
      index += 1;
      continue;
    }
    if (!args[index].startsWith('-')) return args[index];
  }
  return undefined;
}

const packageRoot = path.resolve(fileURLToPath(new URL('..', import.meta.url)));
const server = argument('--server') ?? positionalServer() ?? process.env.REMOE_SIGNAL_SERVER;
const target = '/opt/remoe/signaling-server';

if (!server) {
  throw new Error('用法：npm run deploy -- root@example.com');
}
if (server.includes('\\') || !/^[A-Za-z0-9_.@:[\]-]+$/.test(server)) {
  throw new Error('SSH 服务器参数包含不受支持的字符');
}

for (const relativePath of ['package.json', 'package-lock.json', 'server.mjs', 'lib/database.mjs']) {
  await access(path.join(packageRoot, relativePath), constants.R_OK);
}

const run = (command, args, options = {}) => new Promise((resolve, reject) => {
  const child = spawn(command, args, { stdio: 'inherit', ...options });
  child.on('error', reject);
  child.on('close', code => resolve(code));
});

console.log('Running signaling server tests...');
const testFiles = (await readdir(path.join(packageRoot, 'test')))
  .filter(name => name.endsWith('.test.mjs'))
  .sort()
  .map(name => path.join(packageRoot, 'test', name));
if (!testFiles.length) throw new Error('没有找到 signaling server 测试文件');
if (await run(process.execPath, ['--test', ...testFiles], { cwd: packageRoot }) !== 0) {
  throw new Error('测试失败，已取消部署');
}

const timestamp = new Date().toISOString().replace(/[-:.TZ]/g, '');
const releaseId = `${timestamp}-${randomBytes(4).toString('hex')}`;
const remoteCommand = `set -eu
base='${target}'
release_id='${releaseId}'
release="$base/releases/$release_id"
previous="$(readlink "$base/current")"
switched=0
succeeded=0
rollback() {
  if [ "$succeeded" -eq 1 ]; then return; fi
  echo "Deployment failed; restoring $previous" >&2
  if [ "$switched" -eq 1 ]; then
    ln -sfn "$previous" "$base/current.next"
    mv -Tf "$base/current.next" "$base/current"
    systemctl restart remoe-signaling.service || true
  fi
  rm -rf "$release"
}
trap rollback EXIT HUP INT TERM
mkdir -p "$release"
tar -xzf - -C "$release"
test -s "$release/server.mjs"
test -s "$release/package-lock.json"
test -s "$release/lib/database.mjs"
cd "$release"
npm ci --omit=dev
chown -R root:root "$release"
ln -sfn "releases/$release_id" "$base/current.next"
mv -Tf "$base/current.next" "$base/current"
switched=1
systemctl restart remoe-signaling.service
health_ok=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  if curl --fail --silent http://127.0.0.1:8080/healthz >/dev/null; then
    health_ok=1
    break
  fi
  sleep 1
done
test "$health_ok" -eq 1
succeeded=1
trap - EXIT HUP INT TERM
echo "Activated signaling release $release_id"`;

const archive = spawn('tar', [
  '-czf', '-', '-C', packageRoot,
  'package.json', 'package-lock.json', 'server.mjs', 'lib',
], { stdio: ['ignore', 'pipe', 'inherit'] });
const ssh = spawn('ssh', ['-T', server, remoteCommand], {
  stdio: ['pipe', 'inherit', 'inherit'],
});

archive.stdout.pipe(ssh.stdin);
ssh.stdin.on('error', error => {
  if (error.code !== 'EPIPE') throw error;
});

const waitFor = child => new Promise((resolve, reject) => {
  child.on('error', reject);
  child.on('close', code => resolve(code));
});
const [archiveCode, sshCode] = await Promise.all([waitFor(archive), waitFor(ssh)]);
if (archiveCode !== 0) throw new Error(`tar 打包失败，退出码 ${archiveCode}`);
if (sshCode !== 0) throw new Error(`SSH 部署失败，退出码 ${sshCode}`);
