"""Package installed files with source/build provenance and SHA-256 checksums."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rid", required=True, choices=["win-x64", "linux-x64", "osx-x64", "osx-arm64"])
    parser.add_argument("--stage", type=Path, default=Path("build/stage"))
    parser.add_argument("--toolchain", type=Path, default=Path("build/toolchain-Release.json"))
    args = parser.parse_args()
    version = json.loads(Path("native-build/version.json").read_text())
    subprocess.run(["git", "merge-base", "--is-ancestor", version["upstream_commit"], "HEAD"], check=True)
    expected_tag = "native-" + version["package_version"]
    if os.environ.get("GITHUB_REF_TYPE") == "tag" and os.environ.get("GITHUB_REF_NAME") != expected_tag:
        raise RuntimeError("Release tag must match " + expected_tag)
    filename = {"win-x64": "luau.dll", "linux-x64": "libluau.so", "osx-x64": "libluau.dylib", "osx-arm64": "libluau.dylib"}[args.rid]
    if not (args.stage / "lib" / filename).is_file():
        raise RuntimeError("Missing shared library: " + filename)
    manifest = {
        **version,
        "source_commit": git("rev-parse", "HEAD"),
        "repository": os.environ.get("GITHUB_REPOSITORY", "local"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "runtime_identifier": args.rid,
        "toolchain": json.loads(args.toolchain.read_text()),
        "features": {"interpreter": True, "bytecode_compiler": True, "native_codegen": False,
                     "vector_size": 3, "vector_double": False, "longjmp": True,
                     "static_msvc_crt": args.rid == "win-x64"},
    }
    (args.stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    files = sorted(path for path in args.stage.rglob("*") if path.is_file() and path.name != "SHA256SUMS")
    (args.stage / "SHA256SUMS").write_text("".join(f"{sha256(path)}  {path.relative_to(args.stage).as_posix()}\n" for path in files), encoding="utf-8")
    output = Path("build/packages")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"luau-{version['package_version']}-{args.rid}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(args.stage.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(args.stage).as_posix())
    archive.with_suffix(".zip.sha256").write_text(f"{sha256(archive)}  {archive.name}\n", encoding="utf-8")
    print(archive)


if __name__ == "__main__":
    main()
