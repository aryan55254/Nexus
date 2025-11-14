const statusDiv = document.getElementById("status");
const connectBtn = document.getElementById("connectBtn");
const disconnectBtn = document.getElementById("disconnectBtn");
const messageInput = document.getElementById("messageInput");
const sendBtn = document.getElementById("sendBtn");
const outputDiv = document.getElementById("output");

let ws = null;
const wsUrl = "ws://localhost:8080";

function log(message) {
  outputDiv.textContent += message + "\n";
  outputDiv.scrollTop = outputDiv.scrollHeight;
}

function updateUI(connected) {
  connectBtn.disabled = connected;
  disconnectBtn.disabled = !connected;
  messageInput.disabled = !connected;
  sendBtn.disabled = !connected;
}

connectBtn.onclick = function () {
  log("Connecting to " + wsUrl);
  ws = new WebSocket(wsUrl);

  ws.onopen = function (event) {
    statusDiv.textContent = "Connected";
    statusDiv.style.color = "#28a745";
    log("Connection OPEN");
    updateUI(true);
  };

  ws.onclose = function (event) {
    statusDiv.textContent = "Not Connected";
    statusDiv.style.color = "#dc3545";
    log(`Connection CLOSED: ${event.code} ${event.reason}`);
    ws = null;
    updateUI(false);
  };

  ws.onerror = function (event) {
    statusDiv.textContent = "Connection Error";
    statusDiv.style.color = "#dc3545";
    log("Error: " + event);
    console.error("WebSocket Error: ", event);
  };

  ws.onmessage = function (event) {
    log(event.data);
  };
};

disconnectBtn.onclick = function () {
  if (ws) {
    log("Disconnecting...");
    ws.close();
  }
};

sendBtn.onclick = function () {
  if (ws) {
    const message = messageInput.value;
    if (message) {
      log("You: " + message);
      ws.send(message);
      messageInput.value = "";
    }
  }
};

messageInput.addEventListener("keydown", function (event) {
  if (event.key === "Enter" && !sendBtn.disabled) {
    sendBtn.click();
  }
});
