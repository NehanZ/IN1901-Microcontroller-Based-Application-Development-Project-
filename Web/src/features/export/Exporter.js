// ✅ Add websocket to the array of export type constants
export const [GCODE, THETARHO, SVG, SCARA, WEBSOCKET] = [
  "gcode",
  "thetarho",
  "svg",
  "scara",
  "websocket", // lowercase key used in dropdown logic
]

// ✅ Add label for the dropdown (what appears in the UI)
export const exportTypes = {
  gcode: "GCode",
  thetarho: "Theta Rho",
  svg: "SVG",
  scara: "SCARA GCode (experimental)",
  websocket: "WebSocket (ESP32)", // new option for dropdown
}

// ✅ Exporter option config — nothing else needs to change here unless you want to hide fields for WebSocket
export const exporterOptions = {
  fileName: {
    title: "File name",
    type: "string",
  },
  fileType: {
    title: "Export as",
    type: "dropdown",
    choices: exportTypes,
  },
  polarRhoMax: {
    title: "Maximum rho value (0-1)",
    min: 0,
    max: 1,
  },
  unitsPerCircle: {
    title: "Units per circle",
    type: "number",
  },
  pre: {
    title: "Program start code",
    type: "textarea",
    isVisible: (state) => {
      return state.fileType !== SVG
    },
  },
  post: {
    title: "Program end code",
    type: "textarea",
    isVisible: (exporter, state) => {
      return state.fileType !== SVG
    },
  },
  reverse: {
    title: "Reverse path in the code",
  },
}

// ✅ The Exporter base class stays unchanged
export default class Exporter {
  constructor(props) {
    this.props = props
    this.pre = props.pre
    this.post = props.post
    this.lines = []
    this.indentLevel = 0
    this.digits = 3
  }

  export() {
    this.layers = this.prepareLayers(this.props.layers)

    const allVertices = this.layers.map((layer) => layer.vertices).flat()
    this.computeStats(allVertices, [this])

    this.header()
    this.comment(() => {
      this.line()
      this.keyValueLine("File name", "'" + this.props.fileName + "'")
      this.keyValueLine("File type", this.props.fileType)
      this.line()
    })

    if (this.pre !== "") {
      this.comment("BEGIN PRE")
      this.line(this.pre, this.pre !== "", false)
      this.comment("END PRE")
    }

    this.exportCode()
    this.line()

    if (this.post !== "") {
      this.comment("BEGIN POST")
      this.line(this.post, this.post !== "", false)
      this.comment("END POST")
    }

    this.footer()
    this.line()

    return this.lines
  }

  computeStats(vertices, objects) {
    const values = this.collectStats(vertices)

    Object.keys(values).forEach((variable) => {
      objects.forEach((obj) => {
        obj.pre = this.replaceVariable(obj.pre, variable, values[variable])
        obj.post = this.replaceVariable(obj.post, variable, values[variable])
      })
    })
  }

  prepareLayers(layers) {
    layers = [...layers]

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

  exportCode() {
    this.layers.forEach((layer) => {
      const hasCode = layer.code && layer.code.length > 0

      if (hasCode) {
        this.computeStats(layer.vertices, layer.code)

        this.line("")
        this.actionComment(layer, "BEGIN PRE")

        layer.code.forEach((block) => {
          this.line(block.pre, block.pre !== "", false)
        })
        this.actionComment(layer, "END PRE")
      }

      this.line("")
      this.actionComment(layer, "BEGIN")

      layer.vertices.forEach((vertex) => {
        this.line(this.code(vertex))
      })

      this.actionComment(layer, "END")

      if (hasCode) {
        this.line("")
        this.actionComment(layer, "BEGIN POST")

        layer.code.forEach((block) => {
          this.line(block.post, block.post !== "", false)
        })
        this.actionComment(layer, "END POST")
      }
    })
  }

  // These remain as empty/defaults to be overridden by child classes
  transformVertices(vertices) {
    return vertices
  }

  collectStats() {
    return {}
  }

  header() {}
  footer() {}
  code(/* vertex */) {
    throw "Override this method"
  }

  line(content = "", add = true, sanitize = true) {
    if (add) {
      let padding = ""
      if (this.commenting) {
        padding = this.commentChar + (content.length > 0 ? " " : "")
        for (let i = 0; i < this.indentLevel; i++) {
          padding += "  "
        }
      }

      const preparedContent = sanitize ? this.sanitizeValue(content) : content

      this.lines.push(padding + preparedContent)
    }
  }

  keyValueLine(key, value, add = true) {
    this.line(key + ": " + value, add)
  }

  indent() {
    this.indentLevel++
  }

  dedent() {
    this.indentLevel--
  }

  startComments() {
    this.commenting = true
  }

  endComments() {
    this.commenting = false
  }

  comment(fn) {
    this.startComments()
    typeof fn === "function" ? fn() : this.line(fn)
    this.endComments()
  }

  actionComment(layer, action) {
    const name = layer.name ? ` ${layer.name}` : ""
    this.comment(`${action} ${layer.type}: ${layer.index}${name}`)
  }

  replaceVariable(str, variable, value) {
    if (typeof str !== "string") return str
    const regex = new RegExp(`{${variable}}`, "gi")
    return str.replace(regex, value.toFixed(this.digits))
  }

  sanitizeValue(value) {
    return value.replace("\n", " ")
  }
}
