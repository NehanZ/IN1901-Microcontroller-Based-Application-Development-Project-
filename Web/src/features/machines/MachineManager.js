import React, { useState } from "react"
import Button from "react-bootstrap/Button"
import { Tooltip } from "react-tooltip"
import { FaTrash, FaCopy, FaPlusSquare } from "react-icons/fa"
import { useSelector, useDispatch } from "react-redux"
import {
  selectNumMachines,
  selectCurrentMachineId,
  deleteMachine,
} from "@/features/machines/machinesSlice"
import MachineList from "./MachineList"
import MachineEditor from "./MachineEditor"
import CopyMachine from "./CopyMachine"
import NewMachine from "./NewMachine"

const MachineManager = () => {
  const dispatch = useDispatch()
  const numMachines = useSelector(selectNumMachines)
  const currentMachineId = useSelector(selectCurrentMachineId)

  const canRemove = numMachines > 1
  const [showNewMachine, setShowNewMachine] = useState(false)
  const [showCopyMachine, setShowCopyMachine] = useState(false)

  const toggleNewMachineModal = () => setShowNewMachine(!showNewMachine)
  const toggleCopyMachineModal = () => setShowCopyMachine(!showCopyMachine)
  const handleMachineRemoved = (id) => dispatch(deleteMachine(currentMachineId))

  return (
    <div>
      <MachineEditor />
    </div>
  )
}

export default React.memo(MachineManager)
