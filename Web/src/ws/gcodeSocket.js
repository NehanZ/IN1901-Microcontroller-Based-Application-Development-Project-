let socket = null

export function connectWebSocket(url, onMessage, onOpen) {
  socket = new WebSocket(url)

  socket.onopen = () => {
    console.log("WebSocket connected")
    if (onOpen) {
      onOpen()
    }
  }

  socket.onmessage = (event) => {
    if (onMessage) {
      onMessage(event.data)
    }
  }

  socket.onerror = (error) => {
    console.error("WebSocket error:", error)
  }

  socket.onclose = () => {
    console.log("WebSocket closed")
  }
}

// ✅ ADD THIS FUNCTION EXPORT
export function sendGcodeLine(line) {
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(line)
  } else {
    console.error("WebSocket not connected")
  }
}

// (Optional) disconnect if needed
export function disconnectWebSocket() {
  if (socket) {
    socket.close()
  }
}
