#!/usr/bin/env python3
"""Prepare Dawn for the deko3d renderer (MELEE_NX_GFX=deko3d). Idempotent; touches only Tint's GLSL writer.

    python builder/prepare-deko.py [--dawn ref/dawn]

deko3d clips z to [0, w] and has WebGPU's upper-left origin, so the GLSL writer must neither remap
depth to [-w, w] nor negate position.y. Two options say so (depth_zero_to_one,
disable_position_y_negation); switch/deko/shader_translate.cpp sets both. The edits sit in the GLSL
writer only: the Vulkan (SPIR-V) build does not compile it, so ref/dawn stays valid for both backends.
Adapted from KartPadNX builder/prepare-deko.py (same Dawn pin, 80ee0043).
"""
from pathlib import Path
import argparse
import sys

ROOT = Path(__file__).resolve().parents[1]


def edit(dawn, path, old, new):
    target = dawn / path
    if not target.is_file():
        raise SystemExit(f"Missing Dawn file: {target}")
    text = target.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"Missing expected context in {path}: {old[:100]}")
    target.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def insert_unless(dawn, path, marker, text, present):
    if present not in (dawn / path).read_text(encoding="utf-8"):
        edit(dawn, path, marker, marker + text)


def prepare(dawn):
    options = "src/tint/lang/glsl/writer/common/options.h"
    insert_unless(dawn, options, "std::optional<RangeOffsets> depth_range_offsets;",
                  "\n    bool depth_zero_to_one = false;\n    bool disable_position_y_negation = false;",
                  "bool depth_zero_to_one")
    insert_unless(dawn, options, "                 depth_range_offsets,",
                  "\n                 depth_zero_to_one,\n                 disable_position_y_negation,",
                  "                 disable_position_y_negation,")
    shader_io_h = "src/tint/lang/glsl/writer/raise/shader_io.h"
    insert_unless(dawn, shader_io_h, "std::unordered_set<uint32_t> bgra_swizzle_locations{};",
                  "\n    bool depth_zero_to_one = false;\n    bool disable_position_y_negation = false;",
                  "bool depth_zero_to_one")
    shader_io_cc = "src/tint/lang/glsl/writer/raise/shader_io.cc"
    edit(dawn, shader_io_cc, "auto* new_y = builder.Negation(y);",
         "core::ir::Value* new_y = y;\n            if (!config.disable_position_y_negation) {\n"
         "                new_y = builder.Negation(y);\n            }")
    edit(dawn, "src/tint/lang/glsl/writer/raise/raise.cc",
         "raise::ShaderIOConfig{immediate_data_layout, options.bgra_swizzle_locations}",
         "raise::ShaderIOConfig{immediate_data_layout, options.bgra_swizzle_locations, "
         "options.depth_zero_to_one, options.disable_position_y_negation}")
    edit(dawn, shader_io_cc, "auto* mul = builder.Multiply(2_f, z);\n            auto* new_z = builder.Subtract(mul, w);",
         "core::ir::Value* new_z = z;\n            if (!config.depth_zero_to_one) {\n"
         "                auto* mul = builder.Multiply(2_f, z);\n                new_z = builder.Subtract(mul, w);\n            }")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dawn", type=Path, default=ROOT / "ref/dawn")
    args = parser.parse_args()
    if not (args.dawn / "CMakeLists.txt").is_file():
        sys.exit(f"Dawn source not found: {args.dawn}")
    prepare(args.dawn)
    print("Dawn prepared for deko3d (Tint GLSL writer depth/y options)")
