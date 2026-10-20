"""Render measured G-code differences for the 1MPolygonBridge benchmark.

Requires numpy, matplotlib and shapely. Reads the preserved CPU-only benchmark
outputs; never edits a project or G-code file. This is a deliberately restricted
G-code reader for these linear, absolute-XY, relative-E files, not a printer
interpreter. Unsupported coordinate modes, arcs and units fail explicitly.
"""

import argparse
import hashlib
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle
import numpy as np
from shapely import MultiLineString
from shapely.ops import unary_union


ORANGE = "#cc571d"
BLUE = "#0079b8"
GRAY = "#a9afb6"
CONTEXT = "#dce1e6"
INK = "#1c2b39"
TOLERANCE = 0.002  # mm; two units of the usual 0.001 mm XY output precision.
CASES = {
    "before1": [290],
    "before2": [290],
    "after1": [323, 331],
    "after2": [323, 331],
}


def read_layers(path, wanted):
    x = y = None
    z = 0.
    absolute_xy = True
    relative_e = False
    layer = 0
    role = "Unspecified"
    layers = {}
    for line_number, raw in enumerate(path.open("rb"), 1):
        if raw.startswith(b";LAYER_CHANGE"):
            layer += 1
            if layer > max(wanted):
                break
        elif raw.startswith(b";Z:") and layer in wanted:
            layers.setdefault(str(layer), {"z": float(raw[3:]), "roles": {}})
        elif raw.startswith(b";TYPE:"):
            role = raw[6:].strip().decode()
        elif raw.startswith((b"G", b"M")):
            fields = raw.split(b";", 1)[0].split()
            if not fields:
                continue
            command = fields[0]
            if command == b"G90":
                absolute_xy = True
            elif command == b"G91":
                absolute_xy = False
            elif command == b"M83":
                relative_e = True
            elif command == b"M82":
                relative_e = False
            elif command == b"G20":
                raise ValueError("Inch units are not supported")
            elif command == b"G92":
                values = {p[:1]: float(p[1:]) for p in fields[1:] if p[:1] in (b"X", b"Y", b"Z")}
                x, y, z = values.get(b"X", x), values.get(b"Y", y), values.get(b"Z", z)
            elif command in (b"G0", b"G1", b"G2", b"G3"):
                values = {p[:1]: float(p[1:]) for p in fields[1:] if p[:1] in (b"X", b"Y", b"Z", b"E")}
                if not absolute_xy:
                    raise ValueError("Relative XYZ mode is not supported")
                next_x, next_y = values.get(b"X", x), values.get(b"Y", y)
                z = values.get(b"Z", z)
                if layer in wanted and values.get(b"E", 0.) > 0 and any(c in values for c in (b"X", b"Y", b"Z")):
                    if command in (b"G2", b"G3"):
                        raise ValueError("Arc command encountered; this renderer only handles linear paths")
                    assert relative_e and x is not None and y is not None, (path, line_number)
                    entry = layers.setdefault(str(layer), {"z": z, "roles": {}})
                    rows = entry["roles"].setdefault(role, {"segments": [], "e_mm": 0., "count": 0})
                    rows["count"] += 1
                    rows["e_mm"] += values[b"E"]
                    rows["segments"].append([[x, y], [next_x, next_y]])
                x, y = next_x, next_y
    assert set(layers) == {str(i) for i in wanted}
    return layers


def line_parts(geometry):
    if geometry.is_empty:
        return []
    if geometry.geom_type in ("LineString", "LinearRing"):
        return [np.asarray(geometry.coords)]
    return [part for child in geometry.geoms for part in line_parts(child)]


def differences(a, b):
    aa = unary_union(MultiLineString(a))
    bb = unary_union(MultiLineString(b))
    # Ignore ordering, segmentation and differences below the stated distance.
    unique_a = aa.difference(bb.buffer(TOLERANCE))
    unique_b = bb.difference(aa.buffer(TOLERANCE))
    shared = aa.intersection(bb.buffer(TOLERANCE))
    return unique_a, unique_b, shared


def choose_crop(geometries, size):
    pieces = [part for g in geometries for part in line_parts(g)]
    points, weights = [], []
    for part in pieces:
        for start, end in zip(part[:-1], part[1:]):
            length = np.linalg.norm(end - start)
            n = max(1, int(np.ceil(length / .5)))
            for i in range(n):
                points.append(start + (end - start) * ((i + .5) / n))
                weights.append(length / n)
    points, weights = np.asarray(points), np.asarray(weights)
    assert len(points), "No spatial path difference exceeds the display tolerance"
    candidates = np.unique(np.round(points / 2.) * 2., axis=0)
    scores = [weights[(np.abs(points - c) <= size / 2.).all(axis=1)].sum() for c in candidates]
    center = candidates[int(np.argmax(scores))]
    return [center[0] - size / 2., center[0] + size / 2., center[1] - size / 2., center[1] + size / 2.]


def draw(ax, segments, color, width=.7, alpha=1., zorder=2):
    if len(segments):
        ax.add_collection(LineCollection(segments, colors=color, linewidths=width, alpha=alpha, zorder=zorder))


def decorate(ax, bounds, title):
    ax.set(xlim=bounds[:2], ylim=bounds[2:], aspect="equal", title=title, xlabel="X (mm)", ylabel="Y (mm)")
    ax.tick_params(labelsize=9, colors=INK)
    for spine in ax.spines.values():
        spine.set_color("#c8d0d8")
    ax.grid(alpha=.17, linewidth=.5)
    ax.set_axisbelow(True)


def background(layer):
    return [segment for role, row in layer["roles"].items() for segment in row["segments"]]


def save_figure(fig, output):
    fig.savefig(output.with_suffix(".png"), dpi=170, facecolor="white")
    plt.close(fig)


def render_detail(a, b, role, crop, diffs, title, names, output):
    fig, axes = plt.subplots(1, 3, figsize=(16, 6.4))
    fig.subplots_adjust(left=.055, right=.985, bottom=.19, top=.75, wspace=.28)
    layer = int(output.name.split("-layer-")[1].split("-")[0])
    fig.suptitle(title, fontsize=19, fontweight="bold", x=.055, ha="left", y=.96, color=INK)
    fig.text(.055, .89, f"Layer {layer} · Z = {a['z']:.2f} mm · {role} · identical build and settings within each pair",
             fontsize=11, color=INK)
    fig.text(.055, .842,
             f"Whole-layer role totals: {a['roles'][role]['count']:,} → {b['roles'][role]['count']:,} extrusion commands"
             f"   |   filament E: {a['roles'][role]['e_mm']:.5f} → {b['roles'][role]['e_mm']:.5f} mm",
             fontsize=11, color=INK)
    for ax, data, color, name in zip(axes[:2], (a, b), (ORANGE, BLUE), names):
        draw(ax, background(data), CONTEXT, .45)
        draw(ax, data["roles"][role]["segments"], color, 1.0)
        decorate(ax, crop, name)
    draw(axes[2], background(b), "#eef0f3", .4)
    draw(axes[2], line_parts(diffs[2]), GRAY, .7)
    draw(axes[2], line_parts(diffs[0]), ORANGE, 1.4)
    draw(axes[2], line_parts(diffs[1]), BLUE, 1.2)
    decorate(axes[2], crop, "Spatial difference overlay")
    fig.legend(handles=[
        Line2D([], [], color=ORANGE, lw=2, label=f"{names[0]} only"),
        Line2D([], [], color=BLUE, lw=2, label=f"{names[1]} only"),
        Line2D([], [], color=GRAY, lw=2, label="Coincident selected-role paths"),
    ], loc="lower left", bbox_to_anchor=(.05, .085), ncol=3, frameon=False, fontsize=10)
    fig.text(.055, .055, f"Actual extrusion centerlines, top view. Travel omitted. Overlay distance tolerance: {TOLERANCE:.3f} mm.",
             fontsize=10, color="#506070")
    fig.text(.055, .022, "Colors identify repeated runs, not filament colors. Line thickness is for readability, not deposited bead width.",
             fontsize=9, color="#506070")
    save_figure(fig, output)


def render_overview(a, b, role, crop, output):
    fig, axes = plt.subplots(1, 2, figsize=(11, 9.3))
    fig.subplots_adjust(left=.07, right=.97, bottom=.14, top=.80, wspace=.28)
    fig.suptitle("Where the large original-build difference occurs", fontsize=17, fontweight="bold",
                 x=.07, ha="left", y=.97, color=INK)
    fig.text(.07, .92, f"1MPolygonBridge · layer 290 · Z = {a['z']:.2f} mm · internal solid infill highlighted",
             fontsize=11, color=INK)
    fig.text(.07, .875, "Two repeats of the original build. Dashed box locates the detail image.", fontsize=11, color=INK)
    pts = np.asarray(background(a) + background(b)).reshape(-1, 2)
    low, high = pts.min(axis=0) - 4, pts.max(axis=0) + 4
    bounds = [low[0], high[0], low[1], high[1]]
    for ax, data, color, name in zip(axes, (a, b), (ORANGE, BLUE), ("Original run 1 — outlier", "Original run 2")):
        draw(ax, background(data), CONTEXT, .3)
        draw(ax, data["roles"][role]["segments"], color, .65)
        ax.add_patch(Rectangle((crop[0], crop[2]), crop[1]-crop[0], crop[3]-crop[2],
                               fill=False, edgecolor=INK, linewidth=1.25, linestyle="--", zorder=5))
        decorate(ax, bounds, name)
    fig.text(.07, .075, "Gray: other extrusion paths. Colored: internal solid infill. XY scale is identical in both panels.",
             fontsize=10, color="#506070")
    fig.text(.07, .04, "Top view of actual G-code centerlines; travel omitted. This is not a simulated print or a mesh rendering.",
             fontsize=10, color="#506070")
    save_figure(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=Path("build_logs/bridge_normal_20261020"))
    parser.add_argument("--output", type=Path, default=Path("doc/images/bridge-gcode-variation"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    data, sources = {}, {}
    for label, wanted in CASES.items():
        path = args.evidence / label / "slice/plate_1.gcode"
        metrics = json.loads((path.parent / "metrics.json").read_text())
        expected = json.loads((args.evidence / label / "layer_extrusions.json").read_text())["layers"]
        cache = args.evidence / label / "render_layers.json"
        with path.open("rb") as stream:
            source_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        assert source_hash == metrics["gcodes"][0]["sha256"]
        if cache.exists() and json.loads(cache.read_text())["source_sha256"] == source_hash:
            layers = json.loads(cache.read_text())["layers"]
        else:
            layers = read_layers(path, wanted)
            cache.write_text(json.dumps({"source_sha256": source_hash, "layers": layers}))
        for layer, value in layers.items():
            for role, row in value["roles"].items():
                assert row["count"] == expected[layer][role]["count"]
                assert abs(row["e_mm"] - expected[layer][role]["e_mm"]) < 1e-7
        data[label] = layers
        sources[label] = source_hash
        print(label, "parsed and checked", flush=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11, "text.color": INK,
                         "axes.labelcolor": INK, "axes.titlecolor": INK, "axes.titlesize": 12})
    manifest = {"source_sha256": sources, "overlay_tolerance_mm": TOLERANCE, "figures": []}
    for left, right, layer, role, size, title, names, stem in [
        ("before1", "before2", 290, "Internal solid infill", 22.,
         "Large difference: original-build solid infill",
         ("Original run 1", "Original run 2"), "original-layer-290-detail"),
        ("after1", "after2", 331, "Internal Bridge", 10.,
         "Smaller difference: current-build internal bridge",
         ("Current run 1", "Current run 2"), "current-layer-331-detail"),
        ("after1", "after2", 323, "Internal solid infill", 10.,
         "Smaller difference: current-build solid infill",
         ("Current run 1", "Current run 2"), "current-layer-323-detail"),
    ]:
        a, b = data[left][str(layer)], data[right][str(layer)]
        diffs = differences(a["roles"][role]["segments"], b["roles"][role]["segments"])
        crop = choose_crop(diffs[:2], size)
        render_detail(a, b, role, crop, diffs, title, names, args.output / stem)
        if layer == 290:
            render_overview(a, b, role, crop, args.output / "original-layer-290-overview")
        manifest["figures"].append({"stem": stem, "left": left, "right": right, "layer": layer, "z_mm": a["z"],
                                   "role": role, "crop_mm": crop, "left_only_length_mm": diffs[0].length,
                                   "right_only_length_mm": diffs[1].length, "shared_length_mm": diffs[2].length})
        print(stem, json.dumps(manifest["figures"][-1]), flush=True)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
