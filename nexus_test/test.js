const WebSocket = require("ws");

const NUM_CLIENTS = 1000;
const WS_URL = "ws://localhost:8080";

let connectedClients = 0;
let messagesReceived = 0;

const testStartTime = process.hrtime.bigint();

console.log(`Attempting to connect ${NUM_CLIENTS} clients to ${WS_URL}...`);

function createClient(id) {
  const ws = new WebSocket(WS_URL);

  ws.on("open", function open() {
    connectedClients++;
    console.log(`Client ${id} CONNECTED. (Total: ${connectedClients})`);
    const delay = Math.random() * 3000 + 500;
    setTimeout(() => {
      const message = `Hello from client ${id}!`;
      console.log(`Client ${id} SENDING: ${message}`);
      ws.send(message);
    }, delay);
  });

  ws.on("message", function incoming(data) {
    messagesReceived++;

    const elapsedNanos = process.hrtime.bigint() - testStartTime;
    const elapsedMs = Number(elapsedNanos) / 1_000_000;
    const msgsPerSecond = ((messagesReceived / elapsedMs) * 1000).toFixed(2);
    console.log(
      `Client ${id} RECEIVED: ${data} (Total: ${messagesReceived} in ${elapsedMs.toFixed(
        0
      )}ms | ${msgsPerSecond} msg/s)`
    );
  });

  ws.on("close", function close() {
    connectedClients--;
    console.log(`Client ${id} DISCONNECTED. (Total: ${connectedClients})`);
  });

  ws.on("error", function error(err) {
    console.error(`Client ${id} ERROR: ${err.message}`);
  });
}

for (let i = 0; i < NUM_CLIENTS; i++) {
  setTimeout(() => {
    createClient(i);
  }, i * 20);
}
