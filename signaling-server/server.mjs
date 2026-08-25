import http from 'node:http';
import { WebSocket, WebSocketServer } from 'ws';

const listenHost = process.env.REMOE_SIGNAL_HOST ?? '127.0.0.1';
const listenPort = Number.parseInt(process.env.REMOE_SIGNAL_PORT ?? '8080', 10);
const maxPendingBytes = 2 * 1024 * 1024;
const maxTotalPendingBytes = 32 * 1024 * 1024;
const maxSessions = 1024;
const sessionIdleTimeoutMs = 2 * 60 * 1000;
const sessionPattern = /^[A-Za-z0-9_-]{8,64}$/;
const sessions = new Map();
let totalPendingBytes = 0;

if (!Number.isInteger(listenPort) || listenPort < 1 || listenPort > 65535) {
  throw new Error('REMOE_SIGNAL_PORT must be an integer between 1 and 65535');
}

const server = http.createServer((request, response) => {
  if (request.method === 'GET' && request.url === '/healthz') {
    response.writeHead(200, { 'content-type': 'application/json' });
    response.end(JSON.stringify({ status: 'ok', sessions: sessions.size }));
    return;
  }
  response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
  response.end('Not Found\n');
});

const websocketServer = new WebSocketServer({
  noServer: true,
  maxPayload: 1024 * 1024 + 1024,
  perMessageDeflate: false,
});

function rejectUpgrade(socket, status, reason) {
  socket.write(`HTTP/1.1 ${status} ${reason}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  socket.destroy();
}

function createSession(id) {
  if (sessions.size >= maxSessions) return null;
  const session = {
    host: null,
    client: null,
    pendingHost: [],
    pendingHostBytes: 0,
    pendingClient: [],
    pendingClientBytes: 0,
    lastActivity: Date.now(),
  };
  sessions.set(id, session);
  return session;
}

function peerRole(role) {
  return role === 'host' ? 'client' : 'host';
}

function enqueueFor(session, destinationRole, message) {
  const queueName = destinationRole === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = destinationRole === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  if (session[bytesName] + message.length > maxPendingBytes ||
      totalPendingBytes + message.length > maxTotalPendingBytes) return false;
  session[queueName].push(Buffer.from(message));
  session[bytesName] += message.length;
  totalPendingBytes += message.length;
  return true;
}

function flushPending(session, role, socket) {
  const queueName = role === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = role === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  for (const message of session[queueName]) socket.send(message, { binary: true });
  totalPendingBytes -= session[bytesName];
  session[queueName] = [];
  session[bytesName] = 0;
}

function discardPendingFor(session, destinationRole) {
  const queueName = destinationRole === 'host' ? 'pendingHost' : 'pendingClient';
  const bytesName = destinationRole === 'host' ? 'pendingHostBytes' : 'pendingClientBytes';
  totalPendingBytes -= session[bytesName];
  session[queueName] = [];
  session[bytesName] = 0;
}

function discardPending(session) {
  discardPendingFor(session, 'host');
  discardPendingFor(session, 'client');
}

function removeIfEmpty(id, session) {
  if (!session.host && !session.client &&
      session.pendingHost.length === 0 && session.pendingClient.length === 0) {
    sessions.delete(id);
  }
}

server.on('upgrade', (request, socket, head) => {
  let url;
  try {
    url = new URL(request.url, 'http://localhost');
  } catch {
    rejectUpgrade(socket, 400, 'Bad Request');
    return;
  }
  if (url.pathname !== '/signal') {
    rejectUpgrade(socket, 404, 'Not Found');
    return;
  }
  const id = url.searchParams.get('session') ?? '';
  const role = url.searchParams.get('role') ?? '';
  if (!sessionPattern.test(id) || (role !== 'host' && role !== 'client')) {
    rejectUpgrade(socket, 400, 'Bad Request');
    return;
  }

  let session = sessions.get(id);
  let rejection = null;
  if (role === 'client' &&
      (!session || session.host?.readyState !== WebSocket.OPEN)) {
    rejection = 'invite-not-found';
  }
  if (!session && role === 'host') session = createSession(id);
  if (!session && !rejection) rejection = 'service-unavailable';
  if (session?.[role]) rejection = role === 'client' ? 'invite-in-use' : 'host-in-use';
  websocketServer.handleUpgrade(request, socket, head, (websocket) => {
    websocketServer.emit('connection', websocket, request, { id, role, session, rejection });
  });
});

websocketServer.on('connection', (socket, request, context) => {
  const { id, role, session, rejection } = context;
  if (rejection) {
    socket.send(`error:${rejection}`);
    socket.close(4004, 'Registration rejected');
    return;
  }
  socket.isAlive = true;
  session[role] = socket;
  session.lastActivity = Date.now();
  socket.send('registered');
  flushPending(session, role, socket);

  socket.on('pong', () => {
    socket.isAlive = true;
    session.lastActivity = Date.now();
  });
  socket.on('message', (message, isBinary) => {
    session.lastActivity = Date.now();
    if (!isBinary) {
      socket.close(1003, 'Binary signaling messages required');
      return;
    }
    const destinationRole = peerRole(role);
    const destination = session[destinationRole];
    if (destination?.readyState === WebSocket.OPEN) {
      destination.send(message, { binary: true });
    } else if (!enqueueFor(session, destinationRole, message)) {
      socket.close(1009, 'Pending signaling limit exceeded');
    }
  });
  socket.on('error', (error) => {
    console.error(`WebSocket error for ${role} session ${id}:`, error.message);
  });
  socket.on('close', () => {
    if (session[role] === socket) {
      session[role] = null;
      // Queued messages produced by this connection are no longer valid for
      // a future peer using the same session ID.
      discardPendingFor(session, peerRole(role));
    }
    if (role === 'host') {
      session.client?.close(4001, 'Invite expired');
      discardPending(session);
      sessions.delete(id);
      return;
    }
    removeIfEmpty(id, session);
  });
});

const heartbeat = setInterval(() => {
  for (const socket of websocketServer.clients) {
    if (!socket.isAlive) {
      socket.terminate();
      continue;
    }
    socket.isAlive = false;
    socket.ping();
  }
  const expiry = Date.now() - sessionIdleTimeoutMs;
  for (const [id, session] of sessions) {
    if (session.lastActivity >= expiry) continue;
    session.host?.terminate();
    session.client?.terminate();
    discardPending(session);
    sessions.delete(id);
  }
}, 30_000);
heartbeat.unref();

server.listen(listenPort, listenHost, () => {
  console.log(`remoe signaling server listening on ${listenHost}:${listenPort}`);
});

function shutdown() {
  clearInterval(heartbeat);
  for (const socket of websocketServer.clients) socket.close(1001, 'Server shutting down');
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 5_000).unref();
}

process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
