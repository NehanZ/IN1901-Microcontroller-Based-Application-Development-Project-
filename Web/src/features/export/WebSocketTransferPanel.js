import React, { useState, useRef } from "react"
import { useSelector } from "react-redux"
import { Button, ProgressBar } from "react-bootstrap"
import { selectCurrentMachine } from "@/features/machines/machinesSlice"
import { selectLayersForExport } from "@/features/layers/layersSlice"
import WebSocketExporter from "./WebSocketExporter"

const WebSocketTransferPanel = () => {
  const machine = useSelector(selectCurrentMachine)
  const layers = useSelector(selectLayersForExport)

  const [isSending, setIsSending] = useState(false)
  const [progress, setProgress] = useState(0)
  const [lineInfoState, setLineInfoState] = useState({ current: 0, total: 0 })

  const exporterRef = useRef(null)

  const setLineInfo = (current, total) => {
    setLineInfoState({ current, total })
  }

  const updateProgress = (value) => {
    setProgress(value)
    setLineInfoState((prev) => ({
      ...prev,
      current: Math.round((value / 100) * prev.total),
    }))
  }

  const props = {
    layers,
    setIsSending,
    setProgress: updateProgress,
    setLineInfo,
    onMessage: () => {},
    offsetX:
      machine.type === "rectangular"
        ? (machine.minX + machine.maxX) / 2
        : machine.maxRadius,
    offsetY:
      machine.type === "rectangular"
        ? (machine.minY + machine.maxY) / 2
        : machine.maxRadius,
    width:
      machine.type === "rectangular"
        ? machine.maxX - machine.minX
        : 2.0 * machine.maxRadius,
    height:
      machine.type === "rectangular"
        ? machine.maxY - machine.minY
        : 2.0 * machine.maxRadius,
    maxRadius:
      machine.type === "rectangular"
        ? Math.sqrt(
            Math.pow((machine.maxX - machine.minX) / 2, 2) +
              Math.pow((machine.maxY - machine.minY) / 2, 2)
          )
        : machine.maxRadius,
  }

  const handleStartTransfer = () => {
    exporterRef.current = new WebSocketExporter(props)
    const lines = exporterRef.current.export()
    setLineInfo(0, lines.length)
  }

  const handleCancelTransfer = () => {
    exporterRef.current?.cancel()
    setLineInfo(0, 0)
  }

  return (
    <div
      style={{
        padding: "1rem",
        borderTop: "1px solid #ccc",
        backgroundColor: "#fafafa",
      }}
    >

      <div className="mb-3">
        <Button
          variant="primary"
          onClick={handleStartTransfer}
          disabled={isSending}
        >
          {isSending ? "Sending..." : "Send to Table"}
        </Button>{" "}
        {isSending && (
          <Button variant="outline-danger" onClick={handleCancelTransfer}>
            Cancel
          </Button>
        )}
      </div>

      {isSending && (
        <>
          <ProgressBar animated striped now={progress} label={`${progress}%`} />
          <div className="mt-2 text-muted">
            Line {lineInfoState.current} / {lineInfoState.total}
          </div>
        </>
      )}
    </div>
  )
}

export default WebSocketTransferPanel
