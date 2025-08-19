// NewLayer.js - Fixed to use correct fancyText shape name
import React, { useState, useRef } from "react"
import { useSelector, useDispatch } from "react-redux"
import { selectCurrentMachine } from "@/features/machines/machinesSlice"
import Select from "react-select"
import Col from "react-bootstrap/Col"
import Row from "react-bootstrap/Row"
import Form from "react-bootstrap/Form"
import Button from "react-bootstrap/Button"
import S from "react-switch"
const Switch = S.default ? S.default : S // Fix: https://github.com/vitejs/vite/issues/2139
import Modal from "react-bootstrap/Modal"
import {
  getShapeSelectOptions,
  getShape,
} from "@/features/shapes/shapeFactory"
import Layer from "./Layer"
import { addLayerWithRandomValues } from "./layersSlice"

// Fixed: Use correct shape name "fancyText" instead of "fancy_text"
const getFancyTextShape = () => {
  try {
    return getShape("fancyText")
  } catch {
    // Fallback to polygon if fancyText doesn't exist
    return getShape("polygon")
  }
}

const defaultShape = getFancyTextShape()
const customStyles = {
  control: (base) => ({
    ...base,
    height: 55,
    minHeight: 55,
  }),
}

const NewLayer = ({ toggleModal, showModal }) => {
  const dispatch = useDispatch()
  const selectRef = useRef()
  const selectOptions = getShapeSelectOptions()
  const [type, setType] = useState(defaultShape.type)
  const [name, setName] = useState(defaultShape.label)
  const [randomize, setRandomize] = useState(false)
  const selectedShape = getShape(type)

  const selectedOption = {
    value: selectedShape.id,
    label: selectedShape.label,
  }
  const machineState = useSelector(selectCurrentMachine)

  const handleNameFocus = (event) => {
    event.target.select()
  }

  const handleChangeNewType = (selected) => {
    const shape = getShape(selected.value)

    setType(selected.value)
    setName(shape.label.toLowerCase())
  }

  const handleChangeNewName = (event) => {
    setName(event.target.value)
  }

  const handleInitialFocus = () => {
    selectRef.current.focus()
  }

  const handleRandomizeChange = (value) => {
    setRandomize(value)
  }

  const onLayerAdded = (event) => {
    event.preventDefault()

    const layer = new Layer(type)
    const layerProps = { machine: machineState }
    const attrs = layer.getInitialState(layerProps)

    attrs.name = name
    dispatch(
      addLayerWithRandomValues({
        layer: attrs,
        randomize,
      }),
    )
    toggleModal()
  }

  return (
    <Modal
      show={showModal}
      onHide={toggleModal}
      onEntered={handleInitialFocus}
    >
      <Modal.Header closeButton>
        <Modal.Title>Create new layer</Modal.Title>
      </Modal.Header>

      <Form onSubmit={onLayerAdded}>
        <Modal.Body>
          <Row className="align-items-center">
            <Col sm={5}>Type</Col>
            <Col sm={7}>
              <Select
                ref={selectRef}
                value={selectedOption}
                onChange={handleChangeNewType}
                styles={customStyles}
                maxMenuHeight={305}
                options={selectOptions}
              />
            </Col>
          </Row>
          <Row className="align-items-center mt-2">
            <Col sm={5}>Name</Col>
            <Col sm={7}>
              <Form.Control
                value={name}
                onFocus={handleNameFocus}
                onChange={handleChangeNewName}
              />
            </Col>
          </Row>
          {selectedShape.randomizable && (
            <Row className="align-items-center mt-2">
              <Col sm={5}>Randomize values</Col>
              <Col sm={7}>
                <Switch
                  checked={randomize}
                  onChange={handleRandomizeChange}
                />
              </Col>
            </Row>
          )}
        </Modal.Body>

        <Modal.Footer>
          <Button
            id="new-layer-close"
            variant="light"
            onClick={toggleModal}
          >
            Cancel
          </Button>
          <Button
            id="new-layer-add"
            variant="primary"
            type="submit"
          >
            Create
          </Button>
        </Modal.Footer>
      </Form>
    </Modal>
  )
}

export default NewLayer