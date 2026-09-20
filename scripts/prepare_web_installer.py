#!/usr/bin/env python3
"""Prepare the static ESP Web Tools installer for GitHub Pages."""

from __future__ import annotations

import argparse
import configparser
import json
import re
import shutil
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parent.parent
WEB_SOURCE = PROJECT_ROOT / "web"
DEFAULT_FIRMWARE = PROJECT_ROOT / ".pio" / "build" / "photopainter" / "firmware.factory.bin"
VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
TAG_PATTERN = re.compile(r"^v(?P<version>\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?)$")


def read_firmware_version() -> str:
    config = configparser.ConfigParser(interpolation=None)
    config.read(PROJECT_ROOT / "platformio.ini")

    version = config.get("env:photopainter", "custom_app_version", fallback="").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(
            "platformio.ini custom_app_version must look like 0.1.0 or 0.1.0-beta.1"
        )
    return version


def build_manifest(version: str) -> dict[str, object]:
    return {
        "name": "TRMNL for PhotoPainter",
        "version": version,
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "improv": False,
                "parts": [{"path": "photopainter.factory.bin", "offset": 0}],
            }
        ],
    }


def load_releases(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("GitHub releases JSON must contain an array")

    # `gh api --paginate --slurp` returns one array per API page.
    if payload and all(isinstance(page, list) for page in payload):
        return [release for page in payload for release in page]
    return payload


def prepare_release_catalog(output: Path, releases_json: Path, firmware_dir: Path) -> None:
    shutil.copytree(WEB_SOURCE, output, dirs_exist_ok=True)
    catalog: list[dict[str, object]] = []

    for release in load_releases(releases_json):
        if not isinstance(release, dict) or release.get("draft", False):
            continue

        tag = release.get("tag_name", "")
        match = TAG_PATTERN.fullmatch(tag) if isinstance(tag, str) else None
        if not match:
            continue

        source = firmware_dir / tag / "photopainter.factory.bin"
        if not source.is_file():
            # Older releases without a browser-flashable factory image cannot be used.
            continue

        version = match.group("version")
        release_dir = output / "firmware" / tag
        release_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, release_dir / "photopainter.factory.bin")
        (release_dir / "manifest.json").write_text(
            json.dumps(build_manifest(version), indent=2) + "\n", encoding="utf-8"
        )
        catalog.append(
            {
                "tag": tag,
                "version": version,
                "prerelease": bool(release.get("prerelease", False)),
                "published_at": release.get("published_at"),
                "manifest": f"firmware/{tag}/manifest.json",
            }
        )

    stable = next((release for release in catalog if not release["prerelease"]), None)
    if stable is None:
        raise ValueError("No stable release with photopainter.factory.bin was found")

    # Keep a root manifest/image for compatibility and as the no-JavaScript default.
    stable_dir = output / "firmware" / str(stable["tag"])
    shutil.copy2(stable_dir / "photopainter.factory.bin", output / "photopainter.factory.bin")
    shutil.copy2(stable_dir / "manifest.json", output / "manifest.json")
    (output / "releases.json").write_text(
        json.dumps({"default": stable["tag"], "releases": catalog}, indent=2) + "\n",
        encoding="utf-8",
    )
    (output / ".nojekyll").touch()


def prepare(output: Path, firmware: Path, version: str | None = None) -> None:
    if not firmware.is_file():
        raise FileNotFoundError(f"Factory image not found at {firmware}. Run `pio run` first.")

    shutil.copytree(WEB_SOURCE, output, dirs_exist_ok=True)
    shutil.copy2(firmware, output / "photopainter.factory.bin")
    firmware_version = version or read_firmware_version()
    if not VERSION_PATTERN.fullmatch(firmware_version):
        raise ValueError("version must look like 0.1.0 or 0.1.0-beta.1")
    manifest = build_manifest(firmware_version)
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (output / ".nojekyll").touch()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=PROJECT_ROOT / "dist" / "web-installer",
        help="Directory to populate (default: dist/web-installer)",
    )
    parser.add_argument(
        "--firmware",
        type=Path,
        default=DEFAULT_FIRMWARE,
        help="Path to the PlatformIO factory image",
    )
    parser.add_argument(
        "--version",
        help="Firmware version for the manifest (defaults to platformio.ini)",
    )
    parser.add_argument(
        "--releases-json",
        type=Path,
        help="GitHub releases API JSON used to build a multi-version catalog",
    )
    parser.add_argument(
        "--firmware-dir",
        type=Path,
        help="Directory containing <tag>/photopainter.factory.bin for the catalog",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    if args.releases_json:
        if not args.firmware_dir:
            raise ValueError("--firmware-dir is required with --releases-json")
        prepare_release_catalog(
            args.output.resolve(), args.releases_json.resolve(), args.firmware_dir.resolve()
        )
    else:
        prepare(args.output.resolve(), args.firmware.resolve(), args.version)
