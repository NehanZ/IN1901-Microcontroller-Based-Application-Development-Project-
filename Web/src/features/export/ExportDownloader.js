import { ProgressBar, Spinner } from "react-bootstrap"
import React, { useRef, useState, useEffect } from "react"
import { useSelector, useDispatch } from "react-redux"
import Col from "react-bootstrap/Col"
import Row from "react-bootstrap/Row"
import Button from "react-bootstrap/Button"
import Modal from "react-bootstrap/Modal"
import { downloadFile } from "@/common/util"
import DropdownOption from "@/components/DropdownOption"
import InputOption from "@/components/InputOption"
import CheckboxOption from "@/components/CheckboxOption"
import { selectLayersForExport } from "@/features/layers/layersSlice"
import {
  selectExporterState,
  updateExporter,
} from "@/features/export/exporterSlice"
import { selectCurrentMachine } from "@/features/machines/machinesSlice"
import GCodeExporter from "./GCodeExporter"
import ScaraGCodeExporter from "./ScaraGCodeExporter"
import SvgExporter from "./SvgExporter"
import ThetaRhoExporter from "./ThetaRhoExporter"
import WebSocketExporter from "./WebSocketExporter"
import {
  exporterOptions,
  GCODE,
  THETARHO,
  SVG,
  SCARA,
  WEBSOCKET,
} from "./Exporter"

const exporters = {
  [GCODE]: GCodeExporter,
  [THETARHO]: ThetaRhoExporter,
  [SVG]: SvgExporter,
  [SCARA]: ScaraGCodeExporter,
  [WEBSOCKET]: WebSocketExporter,
}

const ExportDownloader = ({ showModal, toggleModal }) => {
  const dispatch = useDispatch()
  const machine = useSelector(selectCurrentMachine)
  const exporterState = useSelector(selectExporterState)
  const {
    fileType,
    fileName,
    pre,
    post,
    polarRhoMax,
    unitsPerCircle,
    reverse,
  } = exporterState

  // Prepare props early so useEffect and other hooks can access it
  const props = {
    fileType,
    fileName,
    pre,
    post,
    polarRhoMax,
    unitsPerCircle,
    reverse,
    offsetX:
      machine.type === "rectangular"
        ? (machine.minX + machine.maxX) / 2.0
        : machine.maxRadius,
    offsetY:
      machine.type === "rectangular"
        ? (machine.minY + machine.maxY) / 2.0
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
              Math.pow((machine.maxY - machine.minY) / 2, 2),
          )
        : machine.maxRadius,
    layers: useSelector(selectLayersForExport),
  }

  const [fields, setFields] = useState({
    fileType,
    fileName,
    pre,
    post,
    polarRhoMax,
    unitsPerCircle,
    reverse,
  })

  const [savedFields] = useState({
    fileType,
    fileName,
    pre,
    post,
    polarRhoMax,
    unitsPerCircle,
    reverse,
  })

  const [wsMessages, setWsMessages] = useState([])
  const [isSending, setIsSending] = useState(false)
  const [progress, setProgress] = useState(0)
  const exporterRef = useRef(null)

  // Update exporter instance whenever fileType or important props change
  useEffect(() => {
    exporterRef.current = new exporters[fields.fileType]({
      ...props,
      onMessage: handleWebSocketMessage,
      setIsSending,
      setProgress,
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fields.fileType, JSON.stringify(props)]) // JSON.stringify used to track changes in props deeply

  useEffect(() => {
    if (!isSending && progress === 100) {
      const timeout = setTimeout(() => setProgress(0), 3000)
      return () => clearTimeout(timeout)
    }
  }, [isSending, progress])

  function handleWebSocketMessage(msg) {
    setWsMessages((prev) => [...prev, msg])
  }

  const handleChange = (value) => {
    setFields((prevFields) => ({
      ...prevFields,
      ...value,
    }))
  }

  const handleDownload = () => {
    if (fields.fileType === WEBSOCKET) {
      exporterRef.current?.export()
      dispatch(updateExporter(fields))
      return
    }

    let name = fields.fileName
    if (!name.includes(".")) {
      name += exporterRef.current?.fileExtension ?? ".txt"
    }
    const type =
      fields.fileType === SVG
        ? "image/svg+xml;charset=utf-8"
        : "text/plain;charset=utf-8"

    exporterRef.current?.export()
    if (exporterRef.current?.lines) {
      downloadFile(name, exporterRef.current.lines.join("\n"), type)
    }
    dispatch(updateExporter(fields))
    toggleModal()
  }

  const handleCancel = () => {
    exporterRef.current?.cancel()
    setFields(savedFields)
    toggleModal()
  }

  return (
    <Modal
      size="lg"
      show={showModal}
      onHide={toggleModal}
    >
      <Modal.Header closeButton>
        <Modal.Title>
          {fields.fileType === WEBSOCKET
            ? "Send G-code to ESP32"
            : "Export to a file"}
        </Modal.Title>
      </Modal.Header>

      <Modal.Body>
        <DropdownOption
          onChange={handleChange}
          options={exporterOptions}
          optionKey="fileType"
          key="fileType"
          index={0}
          data={props}
        />

        {fields.fileType === SCARA && (
          <Row>
            <Col sm={5}></Col>
            <Col
              sm={7}
              className="mb-2"
            >
              <a
                target="_blank"
                rel="noopener noreferrer"
                href="https://github.com/jeffeb3/sandify/wiki/Scara-GCode"
              >
                Read more
              </a>{" "}
              about SCARA GCode.
            </Col>
          </Row>
        )}

        {fields.fileType !== WEBSOCKET && (
          <InputOption
            onChange={handleChange}
            options={exporterOptions}
            key="fileName"
            optionKey="fileName"
            index={1}
            data={props}
          />
        )}

        {(fields.fileType === THETARHO || fields.fileType === SCARA) && (
          <InputOption
            onChange={handleChange}
            options={exporterOptions}
            key="polarRhoMax"
            optionKey="polarRhoMax"
            index={2}
            data={props}
          />
        )}

        {fields.fileType === SCARA && (
          <InputOption
            onChange={handleChange}
            options={exporterOptions}
            key="unitsPerCircle"
            optionKey="unitsPerCircle"
            index={2}
            data={props}
          />
        )}

        <InputOption
          onChange={handleChange}
          options={exporterOptions}
          key="pre"
          optionKey="pre"
          index={3}
          data={props}
        />

        <InputOption
          onChange={handleChange}
          options={exporterOptions}
          key="post"
          optionKey="post"
          index={4}
          data={props}
        />

        <Row>
          <Col sm={5}></Col>
          <Col sm={7}>
            See the{" "}
            <a
              target="_blank"
              rel="noopener noreferrer"
              href="https://github.com/jeffeb3/sandify/wiki#export-variables"
            >
              wiki{" "}
            </a>
            for details on program export variables.
          </Col>
        </Row>

        <div className="mt-2">
          <CheckboxOption
            onChange={handleChange}
            options={exporterOptions}
            optionKey="reverse"
            key="reverse"
            index={5}
            data={props}
          />
        </div>

        {fields.fileType === WEBSOCKET && (
          <>
            <div
              style={{
                backgroundColor: "#f1f1f1",
                border: "1px solid #ccc",
                borderRadius: "6px",
                padding: "10px",
                marginTop: "1rem",
                maxHeight: "150px",
                overflowY: "auto",
                fontFamily: "monospace",
                fontSize: "0.85rem",
              }}
            >
              <strong>ESP32 Messages:</strong>
              <ul style={{ margin: 0, paddingLeft: "1rem" }}>
                {wsMessages.map((msg, i) => (
                  <li key={i}>{msg}</li>
                ))}
              </ul>
            </div>
            {isSending && (
              <div className="mt-3">
                <ProgressBar
                  now={progress}
                  label={`${progress}%`}
                  animated
                  striped
                />
                <p className="mt-2">Sending G-code to ESP32...</p>
              </div>
            )}
          </>
        )}
      </Modal.Body>

      <Modal.Footer>
        <Button
          id="code-close"
          variant="light"
          onClick={handleCancel}
          disabled={!isSending}
        >
          {isSending ? "Cancel Transfer" : "Cancel"}
        </Button>

        <Button
          id="code-download"
          variant="primary"
          onClick={handleDownload}
          disabled={isSending}
        >
          {fields.fileType === WEBSOCKET
            ? isSending
              ? "Sending..."
              : "Transfer"
            : "Download"}
        </Button>
      </Modal.Footer>
    </Modal>
  )
}

export default React.memo(ExportDownloader)
