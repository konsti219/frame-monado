#!/usr/bin/env python3
# SPDX-License-Identifier: BSL-1.0
"""Generate physical input lookup entries from Monado's OpenXR bindings."""
import json
import sys
from pathlib import Path

aux, output = map(Path, sys.argv[1:])
sys.path.insert(0, str(aux))
from bindings import Bindings
from rebinding import get_expanded_rebinding_entries, iter_rebinding_bindings

profiles = Bindings.load_and_parse(aux / "bindings.json").profiles
native = {}
families = set()
for profile in profiles:
    entries = {}
    for identifier in profile.identifiers:
        members = tuple(sorted({component.monado_binding for component in identifier.components
                                if component.component_name in ("click", "value", "force") and component.monado_binding}))
        if len(members) > 1:
            families.add((profile.monado_device_enum, members))
        for component in identifier.components:
            if not component.monado_binding or not component.monado_binding.startswith("XRT_INPUT_"):
                continue
            paths = component.get_full_openxr_paths()
            if component.component_name == "position":
                paths.append(component.subaction_path + component.identifier_json_path)
            for path in paths:
                entries[path] = component.monado_binding
    native[profile.name] = (profile.monado_device_enum, entries)

rows = set()
for name, (device, entries) in native.items():
    for path, binding in entries.items():
        rows.add((device, name, path, binding))
rebindings = {}
for path in sorted((aux / "rebindings").glob("*.json")):
    rebindings.update(json.loads(path.read_text()))
for name, rebinding in rebindings.items():
    if rebinding.get("helper") or rebinding.get("target_profile") not in native:
        continue
    device, entries = native[rebinding["target_profile"]]
    expanded = dict(rebinding, expanded_bindings=get_expanded_rebinding_entries(name, rebinding, rebindings))
    for source, target in iter_rebinding_bindings(expanded):
        if target in entries:
            rows.add((device, rebinding["source_profile"], source, entries[target]))

with output.open("w") as f:
    f.write("// Generated from OpenXR bindings and rebindings.\n")
    f.write("static const struct capture_binding { enum xrt_device_name device; const char *profile, *path; enum xrt_input_name input; } capture_bindings[] = {\n")
    for device, profile, path, binding in sorted(rows):
        f.write(f'    {{{device}, "{profile}", "{path}", {binding}}},\n')
    f.write("};\n")
    f.write("static const struct capture_family { enum xrt_device_name device; uint32_t count; enum xrt_input_name inputs[3]; } capture_families[] = {\n")
    for device, members in sorted(families):
        assert len(members) <= 3
        f.write("    {" + device + ", " + str(len(members)) + ", {" + ", ".join(members) + "}},\n")
    f.write("};\n")
