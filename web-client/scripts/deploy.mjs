import { randomBytes } from 'node:crypto';
import { access } from 'node:fs/promises';
import { constants } from 'node:fs';
import { spawn } from 'node:child_process';
import path from 'node:path';
import process from 'node:process';

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const server = argument('--server') ?? process.env.REMOE_WEB_SERVER;
const target = argument('--target') ?? '/opt/remoe/web-client';

if (!server) {
  throw new Error('使用 --server <user@host> 或 REMOE_WEB_SERVER 指定 SSH 服务器');
}
if (!/^[A-Za-z0-9_.@:[\]-]+$/.test(server)) {
  throw new Error('SSH 服务器参数包含不受支持的字符');
}
if (!/^\/opt\/remoe\/[A-Za-z0-9._/-]+$/.test(target) || target.includes('..')) {
  throw new Error('部署目录必须位于 /opt/remoe/ 内且不能包含 ..');
}

const webRoot = path.resolve('dist');
await access(path.join(webRoot, 'index.html'), constants.R_OK);
await access(path.join(webRoot, 'assets'), constants.R_OK);

const timestamp = new Date().toISOString().replace(/[-:.TZ]/g, '');
const releaseId = `${timestamp}-${randomBytes(4).toString('hex')}`;
const remoteCommand = `set -eu
base='${target}'
release_id='${releaseId}'
release="$base/releases/$release_id"
mkdir -p "$release"
cleanup() { rm -rf "$release"; }
trap cleanup EXIT HUP INT TERM
tar -xzf - -C "$release"
test -s "$release/index.html"
test -d "$release/assets"
mkdir -p "$base"
ln -sfn "releases/$release_id" "$base/current.next"
mv -Tf "$base/current.next" "$base/current"
trap - EXIT HUP INT TERM
echo "Activated web release $release_id"`;

const archive = spawn('tar', ['-czf', '-', '-C', webRoot, '.'], {
  stdio: ['ignore', 'pipe', 'inherit'],
});
const ssh = spawn('ssh', [server, remoteCommand], {
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
