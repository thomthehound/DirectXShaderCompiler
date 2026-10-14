"""Check that AMD raw pixel-input operands belong to the owning SPIR-V entry.

The compiler contract counts instructions separately. This check follows each
InterpolateAtVertexAMD pointer through access chains to its OpVariable and
verifies the variable appears in the correct OpEntryPoint interface only.
"""
from __future__ import annotations

import re


def check_pixel_parameter_interface(assembly: str, owner: str, other: str) -> None:
    entries: dict[str, set[str]] = {}
    definitions: dict[str, list[str]] = {}

    for raw_line in assembly.splitlines():
        line = raw_line.split(";", 1)[0].strip()
        entry = re.match(
            r'^OpEntryPoint\s+Fragment\s+%\S+\s+"([^"]+)"(?:\s+(.*))?$',
            line,
        )
        if entry:
            entries[entry.group(1)] = set((entry.group(2) or "").split())
        defined = re.match(r"^(%\S+)\s+=\s+(Op\w+)(?:\s+(.*))?$", line)
        if defined:
            definitions[defined.group(1)] = [
                defined.group(2), *((defined.group(3) or "").split())
            ]

    if owner not in entries or other not in entries:
        raise ValueError(f"missing pixel entries {owner!r} or {other!r}")
    if owner == other:
        raise ValueError("ownership check requires two different entries")

    def input_variable(pointer: str) -> str:
        seen: set[str] = set()
        while pointer not in seen:
            seen.add(pointer)
            operands = definitions.get(pointer, ())
            if operands and operands[0] == "OpVariable":
                if len(operands) < 3 or operands[2] != "Input":
                    raise ValueError(f"{pointer} is not a shader input variable")
                return pointer
            if operands and operands[0] in ("OpAccessChain", "OpInBoundsAccessChain"):
                if len(operands) < 3:
                    break
                pointer = operands[2]
                continue
            break
        raise ValueError(f"cannot trace AMD pixel-input operand to OpVariable: {pointer}")

    pointers: list[str] = []
    for operands in definitions.values():
        if operands and operands[0] == "OpExtInst":
            if "InterpolateAtVertexAMD" not in operands:
                continue
            index = operands.index("InterpolateAtVertexAMD")
            if index + 1 >= len(operands):
                raise ValueError("AMD interpolation lacks an input pointer")
            pointers.append(operands[index + 1])

    if not pointers:
        raise ValueError("no AMD interpolation instruction was emitted")

    for pointer in pointers:
        variable = input_variable(pointer)
        if variable not in entries[owner]:
            raise ValueError(f"{variable} missing from owner {owner!r} interface")
        if variable in entries[other]:
            raise ValueError(f"{variable} also belongs to {other!r} interface")
