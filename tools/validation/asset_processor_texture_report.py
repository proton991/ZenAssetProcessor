"""Summarize existing M5 evidence without rerunning or changing acceptance gates."""
import argparse
import json
from pathlib import Path
import re

from PIL import Image, ImageDraw

import asset_processor_baseline as baseline
from asset_processor_texture_audit import package, scene_images


def footprint(images, textures):
    upload = gpu = 0
    for texture in textures.values():
        image = images[texture["image"]]
        upload += sum(map(len, image["mips"]))
        if image["authored_mip_count"] or image["format"] in (7, 9, 13, 14):
            gpu += sum(map(len, image["mips"]))
        else:
            width, height = image["width"], image["height"]
            while True:
                gpu += width * height * 4
                if width == 1 and height == 1:
                    break
                width, height = max(1, width // 2), max(1, height // 2)
    return dict(authored_textures=len(textures), unique_images=len(images),
                cpu_owned_pixel_bytes=sum(image["owned_bytes"] for image in images.values()),
                calculated_authored_texture_upload_bytes=upload, calculated_authored_texture_gpu_mip_bytes=gpu)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu-report", type=Path, required=True)
    parser.add_argument("--visual-report", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cpu = json.loads(args.cpu_report.read_text())
    summary = dict(cpu_report=baseline.identity(args.cpu_report), visual_reports=[],
                   memory_note="Calculated authored texture footprints follow TextureManager::LoadSceneTextures, including per-texture duplication and runtime RGBA mip generation. Excludes default/environment textures, allocator alignment and staging pool reservations; VMA lines are measured whole-process GPU allocation, not per-resource upload timing.",
                   cases=[])
    for case in cpu["results"]:
        row = dict(case=case["case"], packages={}, bc7_images=case["audit"]["bc7_images"], rgba_images=case["audit"]["rgba_images"])
        row.update(bc4_images=case["audit"].get("bc4_images", 0), bc5_images=case["audit"].get("bc5_images", 0))
        for profile, identity in case["packages"].items():
            path = Path(identity["path"])
            if baseline.identity(path) != identity:
                raise ValueError("Package identity changed")
            images, textures, _ = scene_images(package(path))
            row["packages"][profile] = dict(package_bytes=identity["bytes"], **footprint(images, textures))
        summary["cases"].append(row)
    for path in args.visual_report:
        visual = json.loads(path.read_text())
        entry = dict(identity=baseline.identity(path), cases=[])
        for case in visual["results"]:
            row = dict(case=case["case"], backend=case["backend"], captures=[])
            for capture in case["captures"]:
                memory = []
                for line in capture["gpu_memory_log"]:
                    memory.append({key: int(value) for key, value in re.findall(r"(\w+_bytes)=(\d+)", line)})
                row["captures"].append(dict(label=capture["label"], memory=memory, publication=capture["publication_log"]))
            entry["cases"].append(row)
        summary["visual_reports"].append(entry)
        rows = [case for case in visual["results"] if case["backend"] == "raster" and case["case"] in ("visual-0", "visual-4", "alpha", "normal", "clearcoat", "shared-role", "shared-flat", "shared-odd", "occlusion")]
        sheet = Image.new("RGB", (1280, 500 * len(rows)), (30, 30, 30))
        draw = ImageDraw.Draw(sheet)
        for index, row in enumerate(rows):
            for column, profile in enumerate(("reference", visual.get("compression_profile", "bc7"))):
                draw.text((column * 640 + 4, index * 500 + 3), f"{row['case']} {profile}", fill="white")
                sheet.paste(Image.open(path.parent / f"{row['case']}-raster-{profile}.png"), (column * 640, index * 500 + 20))
        sheet.save(path.parent / "review-contact-sheet.png")
    baseline.write(args.output, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
