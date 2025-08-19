import Exporter from "./Exporter"
import {
  connectWebSocket,
  sendGcodeLine,
  disconnectWebSocket,
} from "@/ws/gcodeSocket"

export default class WebSocketExporter extends Exporter {
  constructor(props) {
    super(props)
    this.props = props
    this.commentChar = ";"
    this.onMessage = props.onMessage
    this.setIsSending = props.setIsSending
    this.setProgress = props.setProgress
    this.currentLineIndex = 0
    this.linesPending = []
    this.handshakeSent = false
    this.readyToSend = false
    this.setLineInfo = props.setLineInfo
    
    // Add coordinate offsets like GCodeExporter
    this.offsetX = this.props.offsetX
    this.offsetY = this.props.offsetY
    
    // *** NEW: Timeout prevention with ping ***
    this.lastCommandTime = null
    this.commandTimeout = 12000 // 12 seconds before sending ping (ESP32 timeout is 15s)
    this.pingInterval = null
    this.isHoming = false
    this.maxHomingTime = 35000 // 35 seconds max for homing
    this.retryCount = 0 // Track retry attempts for busy responses
  }

  get fileExtension() {
    return ".txt"
  }

  // *** FIXED: Create deep copies and store original coordinates ***
  transformVertices(vertices) {
    return vertices.map((vertex) => {
      // Create a deep copy to avoid modifying original vertices
      const transformedVertex = {
        ...vertex,
        // Use original coordinates if they exist, otherwise use current coordinates
        x: (vertex.originalX !== undefined ? vertex.originalX : vertex.x) + this.offsetX,
        y: (vertex.originalY !== undefined ? vertex.originalY : vertex.y) + this.offsetY,
      }
      
      // Store original coordinates for future transforms
      if (vertex.originalX === undefined) {
        transformedVertex.originalX = vertex.x
        transformedVertex.originalY = vertex.y
      } else {
        transformedVertex.originalX = vertex.originalX
        transformedVertex.originalY = vertex.originalY
      }
      
      return transformedVertex
    })
  }

  // Alternative approach: Override prepareLayers to ensure fresh copies
  prepareLayers(layers) {
    // Create deep copies of layers to avoid modifying original data
    layers = layers.map(layer => ({
      ...layer,
      vertices: layer.vertices.map(vertex => ({ ...vertex }))
    }))

    if (this.props.reverse) {
      layers = layers.reverse()
    }

    layers.forEach((layer, index) => {
      let vertices = this.transformVertices(layer.vertices, index, layers)

      if (this.props.reverse) vertices = vertices.reverse()
      layer.vertices = vertices
    })

    return layers
  }

  // Add stats collection like GCodeExporter
  collectStats(vertices) {
    return {
      minx: Math.min(...vertices.map((v) => v.x)),
      miny: Math.min(...vertices.map((v) => v.y)),
      maxx: Math.max(...vertices.map((v) => v.x)),
      maxy: Math.max(...vertices.map((v) => v.y)),
      startx: vertices[0].x,
      starty: vertices[0].y,
      endx: vertices[vertices.length - 1].x,
      endy: vertices[vertices.length - 1].y,
    }
  }

  code(vertex) {
    // Use G1 (linear move) instead of G0 (rapid move) for drawing
    return `G1 X${vertex.x.toFixed(3)} Y${vertex.y.toFixed(3)}`
  }

  // *** NEW: Start timeout monitoring ***
  startTimeoutMonitoring() {
    this.lastCommandTime = Date.now()
    
    // Clear any existing interval
    if (this.pingInterval) {
      clearInterval(this.pingInterval)
    }
    
    // Check for timeout every 2 seconds
    this.pingInterval = setInterval(() => {
      if (this.lastCommandTime && this.readyToSend) {
        const timeSinceLastCommand = Date.now() - this.lastCommandTime
        
        // If we're approaching timeout (12s), send a ping
        if (timeSinceLastCommand >= this.commandTimeout) {
          console.log(`⏰ ${timeSinceLastCommand}ms since last command, sending ping to prevent timeout...`)
          sendGcodeLine(";ping")
          
          // Reset timer to give ESP32 more time
          this.lastCommandTime = Date.now()
        }
      }
    }, 2000) // Check every 2 seconds
  }

  // *** NEW: Stop timeout monitoring ***
  stopTimeoutMonitoring() {
    if (this.pingInterval) {
      clearInterval(this.pingInterval)
      this.pingInterval = null
    }
    this.lastCommandTime = null
  }

  // *** NEW: Update command time ***
  updateCommandTime() {
    this.lastCommandTime = Date.now()
  }

  export() {
    // *** FIXED: Force complete reset of lines array ***
    this.lines = []
    
    // Call parent export which populates this.lines
    super.export()
    
    console.log(`Generated ${this.lines.length} G-code lines for this export`)

    // *** FIXED: Reset ALL state completely for fresh start ***
    this.stopTimeoutMonitoring() // Stop any existing monitoring
    this.linesPending = [...this.lines] // Clone lines to avoid shared reference
    this.currentLineIndex = 0
    this.handshakeSent = false
    this.readyToSend = false
    this.isHoming = false
    this.retryCount = 0

    if (this.setIsSending) this.setIsSending(true)
    if (this.setProgress) this.setProgress(0)

    const ESP32_IP = "10.142.44.229"
    const WS_PORT = 81
    const wsURL = `ws://${ESP32_IP}:${WS_PORT}`

    connectWebSocket(
      wsURL,
      (msg) => {
        const trimmed = msg.trim().toLowerCase()
        console.log("Received from ESP32:", trimmed)

        // *** NEW: Handle homing-related messages ***
        if (trimmed === "homing_started") {
          console.log("🏠 ESP32 started homing process...")
          this.isHoming = true
          if (this.onMessage) {
            this.onMessage("Homing started... Please wait.")
          }
          // Keep timeout monitoring active during homing
          return
        }

        if (trimmed === "homing_complete") {
          console.log("✅ ESP32 homing completed successfully")
          this.isHoming = false
          if (this.onMessage) {
            this.onMessage("Homing completed successfully!")
          }
          // *** FIXED: Wait longer after homing before sending next command ***
          // ESP32 needs time to settle after homing process
          this.updateCommandTime()
          
          setTimeout(() => {
            if (this.readyToSend) {
              console.log("🏠 Homing settle time complete, sending next line...")
              this.sendNextLine()
            }
          }, 3000) // Wait 3 seconds for ESP32 to fully settle after homing
          
          return
        }

        if (trimmed === "homing_failed") {
          console.log("❌ ESP32 homing failed")
          this.isHoming = false
          if (this.onMessage) {
            this.onMessage("Homing failed. Please check the machine.")
          }
          
          // Stop the transfer process
          this.stopTimeoutMonitoring()
          if (this.setIsSending) this.setIsSending(false)
          disconnectWebSocket()
          return
        }

        // *** NEW: Handle other ESP32 status messages ***
        if (trimmed.startsWith("alarm:")) {
          console.log("⚠️ ESP32 GRBL alarm:", trimmed)
          if (this.onMessage) {
            this.onMessage(`GRBL Alarm: ${trimmed.substring(6)}`)
          }
          
          // Stop the transfer process
          this.stopTimeoutMonitoring()
          if (this.setIsSending) this.setIsSending(false)
          disconnectWebSocket()
          return
        }

        if (trimmed.startsWith("error:")) {
          console.log("❌ ESP32 GRBL error:", trimmed)
          if (this.onMessage) {
            this.onMessage(`GRBL Error: ${trimmed.substring(6)}`)
          }
          
          // Stop the transfer process
          this.stopTimeoutMonitoring()
          if (this.setIsSending) this.setIsSending(false)
          disconnectWebSocket()
          return
        }

        if (trimmed === "busy") {
          console.log("⏳ ESP32 is busy, retrying...")
          if (this.onMessage) {
            this.onMessage("ESP32 is busy. Retrying...")
          }
          
          // *** FIXED: Prevent infinite retry loop and give ESP32 more time ***
          this.retryCount = (this.retryCount || 0) + 1
          
          if (this.retryCount > 5) {
            console.log("❌ Too many busy responses, stopping transfer")
            if (this.onMessage) {
              this.onMessage("ESP32 is too busy. Transfer stopped.")
            }
            
            this.stopTimeoutMonitoring()
            if (this.setIsSending) this.setIsSending(false)
            disconnectWebSocket()
            return
          }
          
          // Exponential backoff: wait longer each retry
          const retryDelay = Math.min(1000 * this.retryCount, 5000) // 1s, 2s, 3s, 4s, 5s max
          
          setTimeout(() => {
            if (this.readyToSend && this.currentLineIndex > 0) {
              console.log(`🔄 Retry attempt ${this.retryCount} after ${retryDelay}ms delay`)
              // Resend the current line (don't decrement index)
              const line = this.linesPending[this.currentLineIndex - 1]
              console.log(`Retrying line ${this.currentLineIndex}/${this.linesPending.length}:`, line)
              sendGcodeLine(line)
              this.updateCommandTime()
            }
          }, retryDelay)
          return
        }

        if (trimmed === "timeout") {
          console.log("⏰ ESP32 command timeout")
          if (this.onMessage) {
            this.onMessage("Command timeout. Stopping transfer.")
          }
          
          // Stop the transfer process
          this.stopTimeoutMonitoring()
          if (this.setIsSending) this.setIsSending(false)
          disconnectWebSocket()
          return
        }

        // *** NEW: Ignore status reports during normal operation ***
        if (trimmed.startsWith("status:")) {
          console.log("📊 ESP32 status:", trimmed.substring(7))
          // Don't process as "ok" - just log it
          return
        }

        // Handshake response
        if (!this.handshakeSent && trimmed === "ok") {
          console.log("ESP32 handshake confirmed.")
          this.handshakeSent = true
          this.readyToSend = true
          this.startTimeoutMonitoring() // Start monitoring after handshake
          this.sendNextLine()
          return
        }

        // Send next line only on "ok" (and not during homing)
        if (this.readyToSend && trimmed === "ok" && !this.isHoming) {
          this.updateCommandTime() // Reset timeout timer
          this.retryCount = 0 // Reset retry counter on successful response
          this.sendNextLine()
        }

        if (this.onMessage) {
          this.onMessage(trimmed)
        }
      },
      () => {
        const handshake = "\r\n"
        sendGcodeLine(handshake)
        console.log("Sent handshake:", handshake)
        // wait for actual "ok" from ESP32
      }
    )

    return this.lines
  }

  sendNextLine() {
    if (this.currentLineIndex >= this.linesPending.length) {
      console.log("✅ All G-code lines sent.")
      this.stopTimeoutMonitoring() // Stop monitoring when done
      if (this.setIsSending) this.setIsSending(false)
      disconnectWebSocket()
      return
    }

    const line = this.linesPending[this.currentLineIndex]
    console.log(
      `Sending line ${this.currentLineIndex + 1}/${this.linesPending.length}:`,
      line,
    )

    sendGcodeLine(line)
    this.updateCommandTime() // Start timeout monitoring for this command
    this.currentLineIndex++

    if (this.setProgress) {
      const percent = Math.round(
        (this.currentLineIndex / this.linesPending.length) * 100,
      )
      this.setProgress(percent)
    }
    if (this.setLineInfo) {
      this.setLineInfo(this.currentLineIndex, this.linesPending.length)
    }
  }

  cancel() {
    console.log("❌ Transfer cancelled.")
    
    // *** FIXED: Complete state reset ***
    this.stopTimeoutMonitoring()
    
    this.readyToSend = false
    this.handshakeSent = false
    this.lines = [] // Clear the lines array
    this.linesPending = []
    this.currentLineIndex = 0
    this.isHoming = false
    this.retryCount = 0
    
    if (this.setIsSending) this.setIsSending(false)
    disconnectWebSocket()
  }
}