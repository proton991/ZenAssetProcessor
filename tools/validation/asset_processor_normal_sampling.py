"""Independent sampled normal error across adjacent mip levels.

Inputs are linear stored channels, before normal reconstruction. This models
normalized coordinates and ideal nearest/bilinear filtering; it is not a
hardware precision or anisotropic-filter bound.
"""
import numpy as np


def sample_channels(image, u, v, linear, repeat_s, repeat_t):
    height, width = image.shape[:2]
    x = u * width - (0.5 if linear else 0.0)
    y = v * height - (0.5 if linear else 0.0)
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = (x - x0, y - y0) if linear else (np.zeros_like(x), np.zeros_like(y))
    x1, y1 = (x0 + 1, y0 + 1) if linear else (x0, y0)
    x0, x1 = (x0 % width, x1 % width) if repeat_s else (x0.clip(0, width - 1), x1.clip(0, width - 1))
    y0, y1 = (y0 % height, y1 % height) if repeat_t else (y0.clip(0, height - 1), y1.clip(0, height - 1))
    top = image[y0, x0] * (1 - fx[..., None]) + image[y0, x1] * fx[..., None]
    bottom = image[y1, x0] * (1 - fx[..., None]) + image[y1, x1] * fx[..., None]
    return top * (1 - fy[..., None]) + bottom * fy[..., None]


def normal_angles(reference, restored, xy):
    left, right = reference * 2 - 1, restored * 2 - 1
    if xy:
        right = np.concatenate((right, np.sqrt(np.maximum(0, 1 - np.sum(right * right, axis=-1)))[..., None]), axis=-1)
    a, b = np.linalg.norm(left, axis=-1), np.linalg.norm(right, axis=-1)
    defined = (a > 1e-12) & (b > 1e-12)
    cosine = np.divide(np.sum(left * right, axis=-1), a * b, out=np.zeros_like(a), where=defined)
    angles = np.where(defined, np.degrees(np.arccos(np.clip(cosine, -1, 1))), 180.0)
    angles[np.all(left == right, axis=-1) & defined] = 0
    return angles


def mip_pair_quality(fine, coarse, decoded_fine, decoded_coarse, xy, sampler):
    result = dict(mip_normal_rms_degrees=0.0, mip_normal_max_degrees=0.0, mip_normal_phases=0)
    if sampler.get("use_mips", True) and sampler.get("mip_linear", True):
        height, width = fine.shape[:2]
        linear = sampler.get("min_linear", True)
        repeat_s, repeat_t = sampler.get("repeat_s", True), sampler.get("repeat_t", True)
        for py in (0.0, 0.25, 0.5, 0.75):
            for px in (0.0, 0.25, 0.5, 0.75):
                sums = np.zeros(3)
                for start in range(0, height, 32):
                    u, v = np.meshgrid((np.arange(width) + 0.5 + px) / width,
                                       (np.arange(start, min(start + 32, height)) + 0.5 + py) / height)
                    a, b, da, db = [sample_channels(image, u, v, linear, repeat_s, repeat_t)
                                    for image in (fine, coarse, decoded_fine, decoded_coarse)]
                    for index, fraction in enumerate((0.25, 0.5, 0.75)):
                        angles = normal_angles(a * (1 - fraction) + b * fraction,
                                               da * (1 - fraction) + db * fraction, xy)
                        sums[index] += np.sum(angles * angles)
                        result["mip_normal_max_degrees"] = max(result["mip_normal_max_degrees"], float(np.max(angles)))
                result["mip_normal_rms_degrees"] = max(result["mip_normal_rms_degrees"], float(np.sqrt(np.max(sums) / (width * height))))
                result["mip_normal_phases"] += 3
    return result
