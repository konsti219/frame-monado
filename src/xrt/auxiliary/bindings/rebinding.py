# Copyright 2020-2025, Collabora, Ltd.
# Copyright 2024-2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0
"""Shared expansion of OpenXR binding aliases."""

def is_rebinding_helper(rebinding):
    return rebinding.get("helper", False) is True


def get_expanded_rebinding_entries(name, rebinding, rebindings, errors=None, stack=None):
    if stack is None:
        stack = []

    if name in stack:
        if errors is not None:
            cycle = " -> ".join(stack + [name])
            errors.append(f"{name}: circular rebinding helper dependency: {cycle}")
        return {}

    entries = {}
    stack = stack + [name]
    for helper_name in rebinding.get("helpers", []):
        helper = rebindings.get(helper_name)
        if helper is None:
            if errors is not None:
                errors.append(f"{name}: unknown rebinding helper {helper_name}")
            continue
        if not is_rebinding_helper(helper):
            if errors is not None:
                errors.append(f"{name}: rebinding helper {helper_name} is not marked as a helper")
            continue
        helper_entries = get_expanded_rebinding_entries(helper_name, helper, rebindings, errors, stack)
        entries.update(helper_entries)

    entries.update(rebinding.get("bindings", {}))
    return entries


def iter_rebinding_bindings(rebinding):
    """Yield enabled source and target rebinding paths."""
    for source, target in rebinding.get("expanded_bindings", rebinding.get("bindings", {})).items():
        if target is False:
            continue
        if target is True:
            yield source, source
        else:
            yield source, target
