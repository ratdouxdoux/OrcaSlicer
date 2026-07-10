#!/usr/bin/env python3
"""
Train and export the first non-neural FullSpectrum Lab/TD Ridge model.

The model is intentionally small and header-only at runtime:

    FilamentMixer Lab baseline + Ridge(features) -> predicted Lab

Training data comes from black-backed sidewall mix records in the supplied
manifest_measurements JSON files. SCE Lab readings are preferred so the model
stays aligned with the current black-backed SCE K/S profile.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import itertools
import json
import math
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np


DEFAULT_MANIFESTS = [
    "Panchroma-G6-8Y9-7M5-0C6-4_SC185_LH0p08_SD10p8_0091B3_AE537F_C8AA0F_868787_20260612_192835_266.manifest_measurements.json",
    "PanchromaSnapseed-W6-1-C5-8-M5-2-Y4-5_SC46_LH0p08_SD10_0091B3_AE537F_FFFF00_FFFFFF_20260625_164811_523.manifest_measurements.json",
    "PanCyanMagentaSnapSpeedYellowTransBlack_SC121_LH0p08_SD10p5_0091B3_AE537F_C8AA0F_000000_20260701_213448_425.manifest_measurements.json",
    "JayoRedFlashBlueSnapWhitePanYellow_SC121_LH0p08_SD10p7_C91818_0000FF_C8AA0F_E4E5E1_20260701_202638_026.manifest_measurements.json",
]

FEATURE_NAMES = [
    "num_active",
    "max_fraction",
    "min_nonzero_fraction",
    "ratio_entropy",
    "fraction_square_sum",
    "weighted_lab_L",
    "weighted_lab_a",
    "weighted_lab_b",
    "weighted_oklab_L",
    "weighted_oklab_a",
    "weighted_oklab_b",
    "weighted_chroma",
    "weighted_hue_sin",
    "weighted_hue_cos",
    "weighted_td_mm",
    "weighted_inverse_td",
    "weighted_opacity",
    "pair_hue_distance",
    "pair_td_difference",
    "pair_log_td_ratio_abs",
    "pair_chroma_difference",
    "pair_lightness_difference",
    "pair_opacity_interaction",
    "pair_ratio_asymmetry",
    "pair_oklab_distance",
    "pair_lab_delta_e",
    "baseline_filamentmixer_L",
    "baseline_filamentmixer_a",
    "baseline_filamentmixer_b",
    "baseline_oklab_L",
    "baseline_oklab_a",
    "baseline_oklab_b",
    "baseline_td_oklab_L",
    "baseline_td_oklab_a",
    "baseline_td_oklab_b",
    "baseline_linear_rgb_L",
    "baseline_linear_rgb_a",
    "baseline_linear_rgb_b",
]

DISPLAY_D65_10_X = 94.811
DISPLAY_D65_10_Y = 100.0
DISPLAY_D65_10_Z = 107.304
LN100 = math.log(100.0)
EPSILON = 1e-12
MIN_FULL_CORRECTION_FRACTION = 1.0 / 6.0
EXPECTED_LAYER_HEIGHT_MM = 0.08


@dataclass(frozen=True)
class Lab:
    L: float
    a: float
    b: float


@dataclass(frozen=True)
class Oklab:
    L: float
    a: float
    b: float


@dataclass(frozen=True)
class LinearRgb:
    r: float
    g: float
    b: float


@dataclass
class Material:
    hex_color: str
    td_mm: float
    lab: Lab
    oklab: Oklab
    fraction: float


@dataclass
class KnownMaterial:
    hex_color: str
    td_mm: float
    lab: Lab
    source_manifest: str
    slot: int


@dataclass
class TrainingSample:
    source_manifest: str
    swatch_id: str
    swatch_type: str
    features: np.ndarray
    target_lab: np.ndarray
    baseline_lab: np.ndarray


def normalize_hex(hex_color: str) -> str:
    if not isinstance(hex_color, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", hex_color):
        raise ValueError(f"invalid hex color: {hex_color!r}")
    return hex_color.upper()


def canonical_recipe_order(
    colors: list[str], td_values: list[float], fractions: list[float]
) -> tuple[list[str], list[float], list[float]]:
    entries = sorted(
        zip(colors, td_values, fractions),
        key=lambda entry: (normalize_hex(entry[0]), round(float(entry[1]), 6), float(entry[2])),
    )
    return (
        [normalize_hex(entry[0]) for entry in entries],
        [float(entry[1]) for entry in entries],
        [float(entry[2]) for entry in entries],
    )


def srgb_to_linear_channel(value: int | float) -> float:
    srgb = float(value) / 255.0 if isinstance(value, int) else float(value)
    return srgb / 12.92 if srgb <= 0.04045 else ((srgb + 0.055) / 1.055) ** 2.4


def linear_to_srgb_channel(value: float) -> float:
    value = min(1.0, max(0.0, value))
    return 12.92 * value if value <= 0.0031308 else 1.055 * (value ** (1.0 / 2.4)) - 0.055


def linear_rgb_from_hex(hex_color: str) -> LinearRgb:
    h = normalize_hex(hex_color)
    return LinearRgb(
        srgb_to_linear_channel(int(h[1:3], 16)),
        srgb_to_linear_channel(int(h[3:5], 16)),
        srgb_to_linear_channel(int(h[5:7], 16)),
    )


def lab_pivot_xyz(value: float) -> float:
    delta = 6.0 / 29.0
    delta3 = delta * delta * delta
    return math.copysign(abs(value) ** (1.0 / 3.0), value) if value > delta3 else value / (3.0 * delta * delta) + 4.0 / 29.0


def lab_from_xyz(x: float, y: float, z: float) -> Lab:
    fx = lab_pivot_xyz(x / DISPLAY_D65_10_X)
    fy = lab_pivot_xyz(y / DISPLAY_D65_10_Y)
    fz = lab_pivot_xyz(z / DISPLAY_D65_10_Z)
    return Lab(116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))


def lab_from_linear_rgb(rgb: LinearRgb) -> Lab:
    x = 100.0 * (0.4124564 * rgb.r + 0.3575761 * rgb.g + 0.1804375 * rgb.b)
    y = 100.0 * (0.2126729 * rgb.r + 0.7151522 * rgb.g + 0.0721750 * rgb.b)
    z = 100.0 * (0.0193339 * rgb.r + 0.1191920 * rgb.g + 0.9503041 * rgb.b)
    return lab_from_xyz(x, y, z)


def lab_from_hex(hex_color: str) -> Lab:
    return lab_from_linear_rgb(linear_rgb_from_hex(hex_color))


def linear_rgb_from_oklab(ok: Oklab) -> LinearRgb:
    l_ = ok.L + 0.3963377774 * ok.a + 0.2158037573 * ok.b
    m_ = ok.L - 0.1055613458 * ok.a - 0.0638541728 * ok.b
    s_ = ok.L - 0.0894841775 * ok.a - 1.2914855480 * ok.b
    l = l_ * l_ * l_
    m = m_ * m_ * m_
    s = s_ * s_ * s_
    return LinearRgb(
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    )


def oklab_from_linear_rgb(rgb: LinearRgb) -> Oklab:
    l = 0.4122214708 * rgb.r + 0.5363325363 * rgb.g + 0.0514459929 * rgb.b
    m = 0.2119034982 * rgb.r + 0.6806995451 * rgb.g + 0.1073969566 * rgb.b
    s = 0.0883024619 * rgb.r + 0.2817188376 * rgb.g + 0.6299787005 * rgb.b
    l_ = math.copysign(abs(l) ** (1.0 / 3.0), l)
    m_ = math.copysign(abs(m) ** (1.0 / 3.0), m)
    s_ = math.copysign(abs(s) ** (1.0 / 3.0), s)
    return Oklab(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    )


def oklab_from_hex(hex_color: str) -> Oklab:
    return oklab_from_linear_rgb(linear_rgb_from_hex(hex_color))


def lab_to_array(lab: Lab) -> np.ndarray:
    return np.array([lab.L, lab.a, lab.b], dtype=float)


def parse_filament_mixer_model(repo_root: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    text = (repo_root / "src" / "libslic3r" / "filament_mixer_model.h").read_text(encoding="utf-8")

    def extract_array(name: str) -> str:
        marker = f"static const {'int' if name == 'POWERS' else 'double'} {name}"
        start = text.index(marker)
        start = text.index("=", start) + 1
        end = text.index("};", start) + 1
        return text[start:end]

    def c_array_to_python(source: str):
        return ast.literal_eval(source.replace("{", "[").replace("}", "]"))

    powers = np.array(c_array_to_python(extract_array("POWERS")), dtype=int)
    coef = np.array(c_array_to_python(extract_array("COEF")), dtype=float)
    intercept = np.array(c_array_to_python(extract_array("INTERCEPT")), dtype=float)
    return powers, coef, intercept


def filament_mixer_lerp(rgb1: tuple[int, int, int], rgb2: tuple[int, int, int], t: float, model) -> tuple[int, int, int]:
    if t <= 0.0:
        return rgb1
    if t >= 1.0:
        return rgb2

    powers, coef, intercept = model
    x = np.array([rgb1[0], rgb1[1], rgb1[2], rgb2[0], rgb2[1], rgb2[2], float(t)], dtype=float)
    features = np.prod(np.where(powers != 0, x[None, :] ** powers, 1.0), axis=1)
    out = features @ coef + intercept
    return tuple(int(min(255.0, max(0.0, v))) for v in out)


def filament_mixer_blend_hex(colors: list[str], fractions: list[float], model) -> str:
    weighted: list[tuple[tuple[int, int, int], float]] = []
    for hex_color, fraction in zip(colors, fractions):
        if fraction <= 0.0:
            continue
        h = normalize_hex(hex_color)
        weighted.append(((int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)), fraction))
    if not weighted:
        return "#000000"
    if len(weighted) == 1:
        r, g, b = weighted[0][0]
        return f"#{r:02X}{g:02X}{b:02X}"

    rgb = weighted[0][0]
    accumulated = weighted[0][1]
    for next_rgb, next_fraction in weighted[1:]:
        new_total = accumulated + next_fraction
        if new_total <= 0.0:
            continue
        rgb = filament_mixer_lerp(rgb, next_rgb, next_fraction / new_total, model)
        accumulated = new_total
    return f"#{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X}"


def reading_lab_average(record: dict, preferred: str = "lab_sce") -> Lab | None:
    measured = record.get("measured") or {}
    values = []
    for reading in measured.get("readings") or []:
        if not isinstance(reading, dict):
            continue
        lab = reading.get(preferred)
        if isinstance(lab, dict) and all(k in lab for k in ("L", "a", "b")):
            values.append((float(lab["L"]), float(lab["a"]), float(lab["b"])))
    if not values:
        return None
    arr = np.array(values, dtype=float)
    return Lab(float(arr[:, 0].mean()), float(arr[:, 1].mean()), float(arr[:, 2].mean()))


def validate_manifest_data(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "fullspectrum.calibration_measurements.v2":
        raise ValueError(f"unsupported measurement schema in {path.name}: {data.get('schema')!r}")

    filaments = {int(f["slot"]): f for f in data.get("primary_filaments", [])}
    primary_colors = {int(slot): normalize_hex(color) for slot, color in (data.get("primary_colors") or {}).items()}
    primary_tds = {int(slot): float(td) for slot, td in (data.get("primary_td_values") or {}).items()}
    if len(filaments) != 4 or set(filaments) != set(primary_colors) or set(filaments) != set(primary_tds):
        raise ValueError(f"incomplete primary filament metadata in {path.name}")

    for slot, filament in filaments.items():
        if normalize_hex(filament["color_hex"]) != primary_colors[slot]:
            raise ValueError(f"primary color mismatch in {path.name}, slot {slot}")
        if abs(float(filament["td"]) - primary_tds[slot]) > 1e-9:
            raise ValueError(f"primary TD mismatch in {path.name}, slot {slot}")

    filename_tokens = path.name.split("_")
    sd_index = next((index for index, token in enumerate(filename_tokens) if token.startswith("SD")), None)
    filename_hexes = filename_tokens[sd_index + 1 : sd_index + 1 + len(filaments)] if sd_index is not None else []
    if len(filename_hexes) == len(filaments) and all(re.fullmatch(r"[0-9A-Fa-f]{6}", value) for value in filename_hexes):
        profile_hexes = [normalize_hex("#" + value) for value in filename_hexes]
        measured_hexes = [primary_colors[slot] for slot in sorted(filaments)]

        def rgb_distance_squared(left: str, right: str) -> int:
            left_rgb = tuple(int(left[index : index + 2], 16) for index in (1, 3, 5))
            right_rgb = tuple(int(right[index : index + 2], 16) for index in (1, 3, 5))
            return sum((a - b) ** 2 for a, b in zip(left_rgb, right_rgb))

        identity = tuple(range(len(filaments)))
        costs = {
            permutation: sum(
                rgb_distance_squared(measured_hexes[slot], profile_hexes[profile_slot])
                for slot, profile_slot in enumerate(permutation)
            )
            for permutation in itertools.permutations(identity)
        }
        best_permutation = min(costs, key=costs.get)
        if best_permutation != identity and costs[best_permutation] + 1000 < costs[identity]:
            mapping = ", ".join(f"{slot + 1}->{profile_slot + 1}" for slot, profile_slot in enumerate(best_permutation))
            raise ValueError(f"measured primary colors are assigned to the wrong profile slots in {path.name}: {mapping}")

    selected_count = 0
    for record in data.get("records", []):
        manifest = record.get("manifest") or {}
        slots = [int(slot) for slot in manifest.get("filament_slots") or []]
        colors = [normalize_hex(color) for color in manifest.get("colors") or []]
        td_values = [float(td) for td in manifest.get("td_values") or []]
        if colors != [primary_colors[slot] for slot in slots]:
            raise ValueError(f"record color/slot mismatch in {path.name}:{manifest.get('swatch_id', '')}")
        if len(td_values) != len(slots) or any(abs(td - primary_tds[slot]) > 1e-9 for td, slot in zip(td_values, slots)):
            raise ValueError(f"record TD/slot mismatch in {path.name}:{manifest.get('swatch_id', '')}")

        if (
            manifest.get("measurement_condition") == "black_backing"
            and manifest.get("measurement_side") == "side"
            and manifest.get("swatch_type") in {"pair_mix", "ternary_mix", "four_color_mix"}
        ):
            selected_count += 1
            readings = (record.get("measured") or {}).get("readings") or []
            if len(readings) != 3 or reading_lab_average(record, "lab_sce") is None or reading_lab_average(record, "lab_sci") is None:
                raise ValueError(f"incomplete three-take SCI/SCE readings in {path.name}:{manifest.get('swatch_id', '')}")
            for reading in readings:
                raw = reading.get("raw_spectrum") or {}
                for mode in ("sce", "sci"):
                    reflectance = ((raw.get(mode) or {}).get("reflectance") or [])
                    if len(reflectance) != 31 or not all(math.isfinite(float(value)) for value in reflectance):
                        raise ValueError(f"invalid {mode.upper()} spectrum in {path.name}:{manifest.get('swatch_id', '')}")

    if selected_count == 0:
        raise ValueError(f"no eligible black-backed sidewall mix records in {path.name}")
    return data


def reference_batch_warnings(manifest_paths: list[Path]) -> list[str]:
    references = []
    for path in manifest_paths:
        data = json.loads(path.read_text(encoding="utf-8"))
        ref = data.get("reference_measurements") or {}
        black = ref.get("black_backing") or {}
        white = ref.get("white_backing") or {}
        values = [float((black if index < 3 else white)[key]) for index, key in enumerate(("L", "a", "b", "L", "a", "b"))]
        references.append((path.name, np.array(values, dtype=float)))

    matrix = np.vstack([values for _, values in references])
    median = np.median(matrix, axis=0)
    warnings = []
    for name, values in references:
        black_delta = float(np.linalg.norm(values[:3] - median[:3]))
        white_delta = float(np.linalg.norm(values[3:] - median[3:]))
        if black_delta > 3.0 or white_delta > 3.0:
            warnings.append(f"{name}: backing reference differs from the cross-profile median (black dE={black_delta:.3f}, white dE={white_delta:.3f})")
    return warnings


def build_known_materials(manifest_paths: Iterable[Path]) -> dict[tuple[str, float], KnownMaterial]:
    known: dict[tuple[str, float], KnownMaterial] = {}
    for path in manifest_paths:
        data = json.loads(path.read_text(encoding="utf-8"))
        filaments = {int(f["slot"]): f for f in data.get("primary_filaments", [])}
        for slot, filament in filaments.items():
            best_record = None
            best_thickness = -1.0
            for record in data.get("records", []):
                manifest = record.get("manifest") or {}
                if manifest.get("swatch_type") != "reflective_anchor":
                    continue
                if manifest.get("measurement_condition") != "black_backing":
                    continue
                slots = manifest.get("filament_slots") or []
                if slots != [slot]:
                    continue
                thickness = float(manifest.get("total_thickness_mm") or 0.0)
                if thickness > best_thickness:
                    best_record = record
                    best_thickness = thickness
            if best_record is None:
                raise ValueError(f"missing black-backed anchor for {path.name}, slot {slot}")
            lab = reading_lab_average(best_record)
            if lab is None:
                raise ValueError(f"missing SCE anchor readings for {path.name}, slot {slot}")
            hex_color = normalize_hex(filament["color_hex"])
            td = float(filament.get("td") or 0.0)
            known[(hex_color, round(td, 4))] = KnownMaterial(hex_color, td, lab, path.name, slot)
    return known


def measured_or_catalog_lab(hex_color: str, td_mm: float, known_materials: dict[tuple[str, float], KnownMaterial]) -> Lab:
    normalized = normalize_hex(hex_color)
    exact = known_materials.get((normalized, round(float(td_mm), 4)))
    if exact is not None:
        return exact.lab
    same_hex = [material for (hex_key, _), material in known_materials.items() if hex_key == normalized]
    if len(same_hex) == 1:
        return same_hex[0].lab
    return lab_from_hex(normalized)


def material_from_input(hex_color: str, td_mm: float, fraction: float, known_materials: dict[tuple[str, float], KnownMaterial]) -> Material:
    lab = measured_or_catalog_lab(hex_color, td_mm, known_materials)
    return Material(
        normalize_hex(hex_color),
        float(td_mm),
        lab,
        oklab_from_hex(hex_color),
        float(fraction),
    )


def hue_chroma(lab: Lab) -> tuple[float, float]:
    chroma = math.hypot(lab.a, lab.b)
    hue = math.atan2(lab.b, lab.a)
    return hue, chroma


def hue_distance(hue_a: float, hue_b: float) -> float:
    delta = abs(hue_a - hue_b)
    return min(delta, 2.0 * math.pi - delta) / math.pi


def opacity_for_layer(td_mm: float, layer_height_mm: float) -> float:
    if td_mm <= EPSILON:
        return 1.0
    return 1.0 - math.exp(-LN100 * layer_height_mm / td_mm)


def lab_from_oklab_mix(ok: Oklab) -> Lab:
    return lab_from_linear_rgb(linear_rgb_from_oklab(ok))


def weighted_linear_rgb_lab(colors: list[str], fractions: list[float]) -> Lab:
    r = g = b = 0.0
    for hex_color, p in zip(colors, fractions):
        rgb = linear_rgb_from_hex(hex_color)
        r += p * rgb.r
        g += p * rgb.g
        b += p * rgb.b
    return lab_from_linear_rgb(LinearRgb(r, g, b))


def weighted_oklab_mix_lab(materials: list[Material], td_weighted: bool) -> Lab:
    weights = []
    for material in materials:
        strength = 1.0 / max(material.td_mm, EPSILON) if td_weighted else 1.0
        weights.append(material.fraction * strength)
    total = sum(weights)
    if total <= EPSILON:
        weights = [material.fraction for material in materials]
        total = sum(weights)
    ok = Oklab(
        sum(weight * material.oklab.L for weight, material in zip(weights, materials)) / total,
        sum(weight * material.oklab.a for weight, material in zip(weights, materials)) / total,
        sum(weight * material.oklab.b for weight, material in zip(weights, materials)) / total,
    )
    return lab_from_oklab_mix(ok)


def featurize(materials: list[Material], layer_height_mm: float, filament_mixer_lab: Lab, colors: list[str], fractions: list[float]) -> np.ndarray:
    active = [m for m in materials if m.fraction > EPSILON]
    total_fraction = sum(m.fraction for m in active)
    if total_fraction <= EPSILON:
        raise ValueError("empty material recipe")
    for material in active:
        material.fraction /= total_fraction

    fractions_nonzero = [m.fraction for m in active]
    max_fraction = max(fractions_nonzero)
    min_fraction = min(fractions_nonzero)
    entropy = -sum(p * math.log(max(p, EPSILON)) for p in fractions_nonzero) / math.log(max(2, len(active)))
    fraction_square_sum = sum(p * p for p in fractions_nonzero)

    hues = []
    chromas = []
    opacities = []
    for material in active:
        hue, chroma = hue_chroma(material.lab)
        opacity = opacity_for_layer(material.td_mm, layer_height_mm)
        hues.append(hue)
        chromas.append(chroma)
        opacities.append(opacity)

    weighted = [
        sum(m.fraction * m.lab.L for m in active),
        sum(m.fraction * m.lab.a for m in active),
        sum(m.fraction * m.lab.b for m in active),
        sum(m.fraction * m.oklab.L for m in active),
        sum(m.fraction * m.oklab.a for m in active),
        sum(m.fraction * m.oklab.b for m in active),
        sum(m.fraction * chroma for m, chroma in zip(active, chromas)),
        sum(m.fraction * math.sin(hue) for m, hue in zip(active, hues)),
        sum(m.fraction * math.cos(hue) for m, hue in zip(active, hues)),
        sum(m.fraction * m.td_mm for m in active),
        sum(m.fraction / max(m.td_mm, EPSILON) for m in active),
        sum(m.fraction * opacity for m, opacity in zip(active, opacities)),
    ]

    pair_acc = [0.0] * 9
    for i in range(len(active)):
        for j in range(i + 1, len(active)):
            a = active[i]
            b = active[j]
            pair_weight = a.fraction * b.fraction
            if pair_weight <= EPSILON:
                continue
            td_ratio = max(a.td_mm, EPSILON) / max(b.td_mm, EPSILON)
            lab_de = math.sqrt((a.lab.L - b.lab.L) ** 2 + (a.lab.a - b.lab.a) ** 2 + (a.lab.b - b.lab.b) ** 2)
            ok_de = math.sqrt((a.oklab.L - b.oklab.L) ** 2 + (a.oklab.a - b.oklab.a) ** 2 + (a.oklab.b - b.oklab.b) ** 2)
            pair_acc[0] += pair_weight * hue_distance(hues[i], hues[j])
            pair_acc[1] += pair_weight * abs(a.td_mm - b.td_mm)
            pair_acc[2] += pair_weight * abs(math.log(td_ratio))
            pair_acc[3] += pair_weight * abs(chromas[i] - chromas[j])
            pair_acc[4] += pair_weight * abs(a.lab.L - b.lab.L)
            pair_acc[5] += pair_weight * opacities[i] * opacities[j]
            pair_acc[6] += pair_weight * abs(a.fraction - b.fraction) / max(a.fraction + b.fraction, EPSILON)
            pair_acc[7] += pair_weight * ok_de
            pair_acc[8] += pair_weight * lab_de

    baseline_oklab = weighted_oklab_mix_lab(active, False)
    baseline_td_oklab = weighted_oklab_mix_lab(active, True)
    baseline_linear_rgb = weighted_linear_rgb_lab(colors, fractions)

    values = [
        float(len(active)),
        max_fraction,
        min_fraction,
        entropy,
        fraction_square_sum,
        *weighted,
        *pair_acc,
        filament_mixer_lab.L,
        filament_mixer_lab.a,
        filament_mixer_lab.b,
        baseline_oklab.L,
        baseline_oklab.a,
        baseline_oklab.b,
        baseline_td_oklab.L,
        baseline_td_oklab.a,
        baseline_td_oklab.b,
        baseline_linear_rgb.L,
        baseline_linear_rgb.a,
        baseline_linear_rgb.b,
    ]
    if len(values) != len(FEATURE_NAMES):
        raise AssertionError(f"feature count mismatch: {len(values)} != {len(FEATURE_NAMES)}")
    return np.array(values, dtype=float)


def collect_samples(manifest_paths: list[Path], known_materials: dict[tuple[str, float], KnownMaterial], filament_mixer_model) -> list[TrainingSample]:
    samples: list[TrainingSample] = []
    for path in manifest_paths:
        data = json.loads(path.read_text(encoding="utf-8"))
        for record in data.get("records", []):
            manifest = record.get("manifest") or {}
            if manifest.get("measurement_condition") != "black_backing":
                continue
            if manifest.get("measurement_side") != "side":
                continue
            if manifest.get("swatch_type") not in {"pair_mix", "ternary_mix", "four_color_mix"}:
                continue
            measured_lab = reading_lab_average(record)
            if measured_lab is None:
                raise ValueError(f"missing SCE Lab readings for {path.name}:{manifest.get('swatch_id', '')}")

            colors = [normalize_hex(c) for c in manifest.get("colors") or []]
            td_values = [float(td) for td in manifest.get("td_values") or []]
            percentages = [float(p) for p in manifest.get("percentages") or []]
            if not colors or len(colors) != len(td_values) or len(colors) != len(percentages):
                raise ValueError(f"invalid recipe arrays for {path.name}:{manifest.get('swatch_id', '')}")
            fraction_sum = sum(max(0.0, p) for p in percentages)
            if fraction_sum <= EPSILON:
                continue
            fractions = [max(0.0, p) / fraction_sum for p in percentages]
            layer_height_mm = float(manifest.get("layer_height_mm") or 0.08)

            colors, td_values, fractions = canonical_recipe_order(colors, td_values, fractions)

            filament_mixer_hex = filament_mixer_blend_hex(colors, fractions, filament_mixer_model)
            filament_mixer_lab = lab_from_hex(filament_mixer_hex)
            materials = [
                material_from_input(hex_color, td_mm, fraction, known_materials)
                for hex_color, td_mm, fraction in zip(colors, td_values, fractions)
            ]
            features = featurize(materials, layer_height_mm, filament_mixer_lab, colors, fractions)
            samples.append(
                TrainingSample(
                    path.name,
                    str(manifest.get("swatch_id") or record.get("swatch_id") or ""),
                    str(manifest.get("swatch_type") or ""),
                    features,
                    lab_to_array(measured_lab),
                    lab_to_array(filament_mixer_lab),
                )
            )
    return samples


def profile_balanced_weights(samples: list[TrainingSample]) -> np.ndarray:
    counts = Counter(sample.source_manifest for sample in samples)
    profile_count = len(counts)
    sample_count = len(samples)
    return np.array(
        [sample_count / (profile_count * counts[sample.source_manifest]) for sample in samples],
        dtype=float,
    )


def fit_ridge(X: np.ndarray, Y_residual: np.ndarray, alpha: float, weights: np.ndarray | None = None):
    if weights is None:
        weights = np.ones(X.shape[0], dtype=float)
    weights = np.asarray(weights, dtype=float)
    weights *= X.shape[0] / weights.sum()
    normalized_weights = weights / weights.sum()

    mean = np.sum(X * normalized_weights[:, None], axis=0)
    scale = np.sqrt(np.sum(((X - mean) ** 2) * normalized_weights[:, None], axis=0))
    scale[scale < 1e-9] = 1.0
    Z = (X - mean) / scale
    y_mean = np.sum(Y_residual * normalized_weights[:, None], axis=0)
    Yc = Y_residual - y_mean
    lhs = Z.T @ (weights[:, None] * Z) + alpha * np.eye(Z.shape[1])
    rhs = Z.T @ (weights[:, None] * Yc)
    coef = np.linalg.solve(lhs, rhs).T
    return mean, scale, coef, y_mean


def predict_ridge(X: np.ndarray, mean: np.ndarray, scale: np.ndarray, coef: np.ndarray, intercept: np.ndarray) -> np.ndarray:
    Z = (X - mean) / scale
    return Z @ coef.T + intercept


def rmse(values: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.sum(values * values, axis=1))))


def leave_one_profile_validation(
    manifest_paths: list[Path], filament_mixer_model, alphas: list[float], profile_conditioned: bool
) -> list[dict]:
    full_known = build_known_materials(manifest_paths) if profile_conditioned else None
    folds = []
    for test_path in manifest_paths:
        train_paths = [path for path in manifest_paths if path != test_path]
        known_materials = full_known if full_known is not None else build_known_materials(train_paths)
        train_samples = collect_samples(train_paths, known_materials, filament_mixer_model)
        test_samples = collect_samples([test_path], known_materials, filament_mixer_model)
        folds.append((test_path.name, train_samples, test_samples))

    rows = []
    for alpha in alphas:
        fold_errors = []
        squared_errors = []
        fold_names = []
        for fold_name, train_samples, test_samples in folds:
            X_train = np.vstack([sample.features for sample in train_samples])
            target_train = np.vstack([sample.target_lab for sample in train_samples])
            baseline_train = np.vstack([sample.baseline_lab for sample in train_samples])
            X_test = np.vstack([sample.features for sample in test_samples])
            target_test = np.vstack([sample.target_lab for sample in test_samples])
            baseline_test = np.vstack([sample.baseline_lab for sample in test_samples])
            mean, scale, coef, intercept = fit_ridge(
                X_train,
                target_train - baseline_train,
                alpha,
                profile_balanced_weights(train_samples),
            )
            predicted = baseline_test + predict_ridge(X_test, mean, scale, coef, intercept)
            errors = predicted - target_test
            fold_errors.append(rmse(errors))
            squared_errors.extend(np.sum(errors * errors, axis=1).tolist())
            fold_names.append(fold_name)
        score = float(np.mean(fold_errors))
        rows.append(
            {
                "alpha": alpha,
                "leave_one_profile_rmse": score,
                "pooled_rmse": float(math.sqrt(np.mean(squared_errors))),
                "fold_rmse": fold_errors,
                "fold_names": fold_names,
            }
        )
    return rows


def nested_leave_one_profile_validation(
    manifest_paths: list[Path], filament_mixer_model, alphas: list[float]
) -> dict:
    fold_errors = []
    fold_alphas = []
    fold_names = []
    squared_errors = []
    for test_path in manifest_paths:
        train_paths = [path for path in manifest_paths if path != test_path]
        inner_rows = leave_one_profile_validation(train_paths, filament_mixer_model, alphas, False)
        selected = min(inner_rows, key=lambda row: row["leave_one_profile_rmse"])
        known_materials = build_known_materials(train_paths)
        train_samples = collect_samples(train_paths, known_materials, filament_mixer_model)
        test_samples = collect_samples([test_path], known_materials, filament_mixer_model)
        X_train = np.vstack([sample.features for sample in train_samples])
        target_train = np.vstack([sample.target_lab for sample in train_samples])
        baseline_train = np.vstack([sample.baseline_lab for sample in train_samples])
        X_test = np.vstack([sample.features for sample in test_samples])
        target_test = np.vstack([sample.target_lab for sample in test_samples])
        baseline_test = np.vstack([sample.baseline_lab for sample in test_samples])
        mean, scale, coef, intercept = fit_ridge(
            X_train,
            target_train - baseline_train,
            selected["alpha"],
            profile_balanced_weights(train_samples),
        )
        errors = baseline_test + predict_ridge(X_test, mean, scale, coef, intercept) - target_test
        fold_errors.append(rmse(errors))
        fold_alphas.append(selected["alpha"])
        fold_names.append(test_path.name)
        squared_errors.extend(np.sum(errors * errors, axis=1).tolist())
    return {
        "leave_one_profile_rmse": float(np.mean(fold_errors)),
        "pooled_rmse": float(math.sqrt(np.mean(squared_errors))),
        "fold_rmse": fold_errors,
        "fold_alphas": fold_alphas,
        "fold_names": fold_names,
    }


def c_string_array(values: list[str], indent: str = "    ") -> str:
    return ",\n".join(f'{indent}"{value}"' for value in values)


def c_double_array(values: Iterable[float], indent: str = "    ") -> str:
    return ", ".join(f"{float(value):.12g}" for value in values)


def c_wrapped_double_array(values: Iterable[float], indent: str = "    ", per_line: int = 6) -> str:
    vals = [f"{float(value):.12g}" for value in values]
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append(indent + ", ".join(vals[i : i + per_line]))
    return ",\n".join(lines)


def write_header(
    output: Path,
    manifest_paths: list[Path],
    known_materials: dict[tuple[str, float], KnownMaterial],
    mean: np.ndarray,
    scale: np.ndarray,
    coef: np.ndarray,
    intercept: np.ndarray,
    alpha: float,
    validation_rows: list[dict],
    conditioned_validation_rows: list[dict],
    nested_validation: dict,
    reference_warnings: list[str],
    model_digest: str,
    samples: list[TrainingSample],
) -> None:
    training_rmse_baseline = rmse(np.vstack([s.baseline_lab for s in samples]) - np.vstack([s.target_lab for s in samples]))
    prediction = np.vstack([s.baseline_lab for s in samples]) + predict_ridge(
        np.vstack([s.features for s in samples]), mean, scale, coef, intercept
    )
    training_rmse_model = rmse(prediction - np.vstack([s.target_lab for s in samples]))
    best_validation = next(row for row in validation_rows if row["alpha"] == alpha)
    conditioned_validation = next(row for row in conditioned_validation_rows if row["alpha"] == alpha)
    feature_matrix = np.vstack([sample.features for sample in samples])
    feature_min = feature_matrix.min(axis=0)
    feature_max = feature_matrix.max(axis=0)

    known_sorted = sorted(known_materials.values(), key=lambda m: (m.hex_color, m.td_mm, m.source_manifest, m.slot))
    source_list = ", ".join(path.name for path in manifest_paths)

    lines = [
        "// This file is generated by scripts/generate_full_spectrum_lab_td_ridge.py.",
        f"// Sources: {source_list}",
        "// Model: FilamentMixer Lab baseline + Ridge residual correction.",
        "// Target: black-backed sidewall SCE Lab mix records.",
        f"// Samples: {len(samples)} mix records; alpha: {alpha:.12g}.",
        f"// Training RMSE DeltaE76: FilamentMixer {training_rmse_baseline:.4f}, Ridge {training_rmse_model:.4f}.",
        f"// Unseen-profile LOPO RMSE DeltaE76: mean {best_validation['leave_one_profile_rmse']:.4f}, pooled {best_validation['pooled_rmse']:.4f}.",
        f"// Profile-conditioned LOPO mean RMSE DeltaE76: {conditioned_validation['leave_one_profile_rmse']:.4f}.",
        f"// Nested unseen-profile LOPO RMSE DeltaE76: mean {nested_validation['leave_one_profile_rmse']:.4f}, pooled {nested_validation['pooled_rmse']:.4f}.",
        f"// Backing-reference warnings: {len(reference_warnings)}.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "namespace Slic3r::FullSpectrumLabTDRidgeModelData {",
        "",
        f"static constexpr std::size_t FEATURE_COUNT = {len(FEATURE_NAMES)};",
        f"static constexpr std::size_t KNOWN_MATERIAL_COUNT = {len(known_sorted)};",
        f'static constexpr const char* MODEL_ID = "fullspectrum_lab_td_ridge_v2_black_sce_{model_digest[:12]}";',
        'static constexpr const char* MODEL_TYPE = "lab_td_ridge_v2";',
        f'static constexpr const char* MODEL_DIGEST = "{model_digest}";',
        'static constexpr const char* PRIMARY_BASELINE = "FilamentMixer";',
        'static constexpr const char* TARGET_SPECULAR_MODE = "SCE";',
        'static constexpr const char* TARGET_BACKING_CONDITION = "black_backing";',
        f"static constexpr double RIDGE_ALPHA = {alpha:.12g};",
        f"static constexpr double VALIDATION_RMSE_DELTA_E76 = {nested_validation['pooled_rmse']:.12g};",
        f"static constexpr double MIN_FULL_CORRECTION_FRACTION = {MIN_FULL_CORRECTION_FRACTION:.12g};",
        f"static constexpr double EXPECTED_LAYER_HEIGHT_MM = {EXPECTED_LAYER_HEIGHT_MM:.12g};",
        f"static constexpr std::size_t REFERENCE_BATCH_WARNING_COUNT = {len(reference_warnings)};",
        "",
        "static constexpr std::array<const char*, FEATURE_COUNT> FEATURE_NAMES = {{",
        c_string_array(FEATURE_NAMES),
        "}};",
        "",
        "static constexpr std::array<double, FEATURE_COUNT> FEATURE_MEAN = {{",
        c_wrapped_double_array(mean),
        "}};",
        "",
        "static constexpr std::array<double, FEATURE_COUNT> FEATURE_SCALE = {{",
        c_wrapped_double_array(scale),
        "}};",
        "",
        "static constexpr std::array<double, FEATURE_COUNT> FEATURE_MIN = {{",
        c_wrapped_double_array(feature_min),
        "}};",
        "",
        "static constexpr std::array<double, FEATURE_COUNT> FEATURE_MAX = {{",
        c_wrapped_double_array(feature_max),
        "}};",
        "",
        "static constexpr std::array<std::array<double, FEATURE_COUNT>, 3> COEFFICIENTS = {{",
    ]
    for row in coef:
        lines.extend(["    {{", c_wrapped_double_array(row, "        "), "    }},"])
    lines.extend(
        [
            "}};",
            "",
            "static constexpr std::array<double, 3> INTERCEPT = {{",
            "    " + c_double_array(intercept),
            "}};",
            "",
            "struct KnownMaterialLab",
            "{",
            "    const char* hex = \"#000000\";",
            "    double td_mm = 0.0;",
            "    std::array<double, 3> lab {};",
            "};",
            "",
            "static constexpr std::array<KnownMaterialLab, KNOWN_MATERIAL_COUNT> KNOWN_MATERIALS = {{",
        ]
    )
    for material in known_sorted:
        lines.append(
            f'    {{"{material.hex_color}", {material.td_mm:.12g}, {{{{{material.lab.L:.12g}, {material.lab.a:.12g}, {material.lab.b:.12g}}}}}}},'
        )
    lines.extend(
        [
            "}};",
            "",
            "} // namespace Slic3r::FullSpectrumLabTDRidgeModelData",
            "",
        ]
    )
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, default=Path("src/libslic3r/FullSpectrumLabTDRidgeModel.h"))
    parser.add_argument("manifests", nargs="*", help="manifest_measurements JSON files")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest_names = args.manifests or DEFAULT_MANIFESTS
    manifest_paths = [(repo_root / name).resolve() for name in manifest_names]
    missing = [str(path) for path in manifest_paths if not path.exists()]
    if missing:
        raise FileNotFoundError("missing manifest file(s): " + ", ".join(missing))

    for path in manifest_paths:
        validate_manifest_data(path)
    if lab_from_hex("#010000").L >= 1.0:
        raise AssertionError("8-bit channel value 1 was interpreted as normalized 1.0")

    output = args.output
    if not output.is_absolute():
        output = repo_root / output

    filament_mixer_model = parse_filament_mixer_model(repo_root)
    known_materials = build_known_materials(manifest_paths)
    samples = collect_samples(manifest_paths, known_materials, filament_mixer_model)
    if len(samples) < len(FEATURE_NAMES) + 3:
        raise RuntimeError(f"not enough training samples: {len(samples)}")

    X = np.vstack([sample.features for sample in samples])
    target = np.vstack([sample.target_lab for sample in samples])
    baseline = np.vstack([sample.baseline_lab for sample in samples])
    residual = target - baseline
    if np.linalg.matrix_rank((X - X.mean(axis=0)) / X.std(axis=0)) != len(FEATURE_NAMES):
        raise RuntimeError("training feature matrix is rank deficient")

    alphas = [0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0]
    validation_rows = leave_one_profile_validation(manifest_paths, filament_mixer_model, alphas, False)
    conditioned_validation_rows = leave_one_profile_validation(manifest_paths, filament_mixer_model, alphas, True)
    selected_validation = min(validation_rows, key=lambda row: row["leave_one_profile_rmse"])
    alpha = selected_validation["alpha"]
    nested_validation = nested_leave_one_profile_validation(manifest_paths, filament_mixer_model, alphas)
    mean, scale, coef, intercept = fit_ridge(X, residual, alpha, profile_balanced_weights(samples))
    reference_warnings = reference_batch_warnings(manifest_paths)
    digest = hashlib.sha256(Path(__file__).read_bytes())
    for path in manifest_paths:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    model_digest = digest.hexdigest()

    output.parent.mkdir(parents=True, exist_ok=True)
    write_header(
        output,
        manifest_paths,
        known_materials,
        mean,
        scale,
        coef,
        intercept,
        alpha,
        validation_rows,
        conditioned_validation_rows,
        nested_validation,
        reference_warnings,
        model_digest,
        samples,
    )

    predicted = baseline + predict_ridge(X, mean, scale, coef, intercept)
    print(f"Wrote {output}")
    print(f"Samples: {len(samples)}")
    print(f"Known materials: {len(known_materials)}")
    print(f"Selected alpha: {alpha:g}")
    print(f"Training RMSE DeltaE76: FilamentMixer={rmse(baseline - target):.4f}, Ridge={rmse(predicted - target):.4f}")
    selected_row = next(row for row in validation_rows if row["alpha"] == alpha)
    print(
        f"Unseen-profile LOPO RMSE DeltaE76: mean={selected_row['leave_one_profile_rmse']:.4f}, "
        f"pooled={selected_row['pooled_rmse']:.4f}"
    )
    print(
        f"Nested unseen-profile LOPO RMSE DeltaE76: mean={nested_validation['leave_one_profile_rmse']:.4f}, "
        f"pooled={nested_validation['pooled_rmse']:.4f}"
    )
    for row in validation_rows:
        if row["alpha"] == alpha:
            print("Selected folds: " + ", ".join(f"{v:.4f}" for v in row["fold_rmse"]))
            break
    for warning in reference_warnings:
        print("WARNING: " + warning)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
