"""C00 module-only scaffold. No shell hooks, linked templates, or runtime dependency."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import uuid

ROOT = Path(__file__).resolve().parent


def safe_files(plan):
    files = plan["files"]
    if not files or len(files) > 20:
        raise ValueError("Invalid file count")
    for relative, text in files.items():
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or "\\" in relative or ":" in relative:
            raise ValueError("Unsafe template path")
        if hashlib.sha256(text.encode("utf-8")).hexdigest() != plan["hashes"][relative]:
            raise ValueError("Plan content changed")
    return files


def main():
    parser = argparse.ArgumentParser(prog="rocks")
    groups = parser.add_subparsers(dest="group", required=True)
    template = groups.add_parser("template")
    commands = template.add_subparsers(dest="command", required=True)
    create = commands.add_parser("plan")
    create.add_argument("kind", choices=["module"])
    create.add_argument("name")
    create.add_argument("--output", required=True)
    create.add_argument("--plan", required=True)
    for command in ("apply", "validate"):
        item = commands.add_parser(command)
        item.add_argument("--plan", required=True)
    args = parser.parse_args()
    if args.command == "plan":
        if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", args.name) is None:
            raise ValueError("Module name must be an identifier")
        target = Path(args.output).resolve() / args.name
        files = {}
        for source in sorted((ROOT / "Templates/module").glob("*.in")):
            relative = source.name.removesuffix(".in").replace("Module", args.name)
            files[relative] = source.read_text(encoding="utf-8").replace("@NAME@", args.name)
        files["module.json"] = json.dumps({"schema": 1, "name": args.name, "module_id": str(uuid.uuid4()), "integration": "explicit-build-registration-required"}, indent=2) + "\n"
        plan = {"schema": 1, "kind": "module", "target": str(target), "files": files,
                "hashes": {k: hashlib.sha256(v.encode("utf-8")).hexdigest() for k, v in files.items()}}
        safe_files(plan)
        with open(args.plan, "x", encoding="utf-8", newline="\n") as output:
            json.dump(plan, output, indent=2)
            output.write("\n")
        return
    plan = json.loads(Path(args.plan).read_text(encoding="utf-8"))
    if plan.get("schema") != 1 or plan.get("kind") != "module":
        raise ValueError("Unsupported template schema")
    files = safe_files(plan)
    target = Path(plan["target"])
    if not target.is_absolute():
        raise ValueError("Target must be absolute")
    target = target.resolve()
    if args.command == "validate":
        for relative, text in files.items():
            path = target / relative
            if path.is_symlink() or path.read_bytes() != text.encode("utf-8"):
                raise ValueError("Generated output differs from plan")
        return
    if target.exists():
        raise ValueError("Target already exists; files will not be overwritten")
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = target.parent / ("." + target.name + ".stage-" + uuid.uuid4().hex)
    staging.mkdir()
    try:
        for relative, text in files.items():
            path = staging / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("x", encoding="utf-8", newline="\n") as output:
                output.write(text)
        if target.exists():
            raise ValueError("Target appeared during staging")
        os.rename(staging, target)
    finally:
        if staging.exists():
            resolved = staging.resolve()
            if resolved.parent != target.parent.resolve() or not resolved.name.startswith("." + target.name + ".stage-"):
                raise ValueError("Refusing cleanup outside owned staging directory")
            shutil.rmtree(resolved)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
