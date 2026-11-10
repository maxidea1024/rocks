"""Generate Rocks object metadata from Clang JSON AST, never by parsing C++ source text."""
from pathlib import Path
import argparse
import json
import os
import tempfile
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Build"))
from build import environment
from audit_warning_policy import command_errors

SCALARS = {
    "bool": "Bool", "unsigned char": "UInt8", "unsigned short": "UInt16",
    "unsigned int": "UInt32", "unsigned long long": "UInt64", "int": "Int32",
    "long long": "Int64", "float": "Float32", "double": "Float64",
    "rocks::Guid": "Guid", "rocks::String": "Utf8String",
}


class MetaError(RuntimeError):
    pass


def constant(node):
    kind = node.get("kind")
    if kind == "ConstantExpr" and "value" in node:
        # Use Clang's evaluated result: descending into -1 or 1+2 would incorrectly
        # select the first literal and silently change the declared schema version.
        try:
            return int(node["value"], 10)
        except ValueError:
            pass
    if kind == "StringLiteral":
        try:
            return json.loads(node["value"])
        except (KeyError, ValueError) as error:
            raise MetaError("annotation strings must be narrow ASCII literals") from error
    if kind == "IntegerLiteral":
        return int(node["value"])
    # Only transparent literal wrappers are accepted. Pointer arithmetic and
    # arbitrary expressions must not masquerade as their first nested string.
    if kind in {"ConstantExpr", "ImplicitCastExpr", "ParenExpr"}:
        children = node.get("inner", [])
        return constant(children[0]) if len(children) == 1 else None
    return None


def annotation(node, marker):
    matches = []
    for child in node.get("inner", []):
        if child.get("kind") != "AnnotateAttr":
            continue
        arguments = [constant(argument) for argument in child.get("inner", [])]
        if arguments and arguments[0] == marker:
            matches.append(arguments[1:])
    if len(matches) > 1:
        raise MetaError(f"duplicate {marker} annotation on {node.get('name', '<unnamed>')}")
    return matches[0] if matches else None


def stable_id(value, symbol):
    if not isinstance(value, str):
        raise MetaError(f"{symbol}: stable ID must be a GUID string literal")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError) as error:
        raise MetaError(f"{symbol}: invalid stable GUID {value!r}") from error
    if parsed.int == 0:
        raise MetaError(f"{symbol}: zero stable GUID is reserved")
    return parsed.hex


def collect(ast):
    records = {}
    used_ids = {}

    def claim(identifier, symbol):
        if identifier in used_ids:
            raise MetaError(f"{symbol}: duplicate stable ID already used by {used_ids[identifier]}")
        used_ids[identifier] = symbol

    def visit(node, scope=(), anonymous=False, templated=False):
        kind = node.get("kind")
        if kind == "NamespaceDecl":
            name = node.get("name")
            for child in node.get("inner", []):
                visit(child, scope + ((name,) if name else ()), anonymous or not name, templated)
            return
        if kind in {"ClassTemplateDecl", "ClassTemplateSpecializationDecl"}:
            templated = True
        if kind == "CXXRecordDecl" and node.get("completeDefinition"):
            name = node.get("name", "")
            qualified = "::".join(scope + (name,))
            mark = annotation(node, "rocks.type")
            if mark is not None:
                if anonymous or templated or not name or len(mark) != 2:
                    raise MetaError(f"{qualified}: anonymous/template/invalid reflected type is unsupported")
                identifier = stable_id(mark[0], qualified)
                claim(identifier, qualified)
                version = mark[1]
                if not isinstance(version, int) or not 1 <= version <= 0xFFFFFFFF:
                    raise MetaError(f"{qualified}: schema version must be 1..UINT32_MAX")
                bases = node.get("bases", [])
                if len(bases) != 1 or bases[0].get("access") != "public" or bases[0].get("isVirtual"):
                    raise MetaError(f"{qualified}: exactly one public nonvirtual Object/reflected base is required")
                base_type = bases[0]["type"]
                base = base_type.get("desugaredQualType", base_type["qualType"])
                fields = []
                access = "private" if node.get("tagUsed") == "class" else "public"
                for member in node.get("inner", []):
                    member_kind = member.get("kind")
                    if member_kind == "AccessSpecDecl":
                        access = member["access"]
                    if member_kind == "CXXMethodDecl" and member.get("name") == "Trace":
                        raise MetaError(f"{qualified}: manual Trace conflicts with complete generated field tracing")
                    if member_kind == "VarDecl" and annotation(member, "rocks.field") is not None:
                        raise MetaError(f"{qualified}: static fields cannot carry reflected field annotations")
                    if member_kind != "FieldDecl":
                        continue
                    symbol = qualified + "::" + member.get("name", "<unnamed>")
                    field_mark = annotation(member, "rocks.field")
                    if field_mark is None or len(field_mark) != 1:
                        raise MetaError(f"{symbol}: every nonstatic field requires a stable field annotation")
                    if access != "public" or member.get("isBitfield") or not member.get("name"):
                        raise MetaError(f"{symbol}: only named public non-bitfield members are supported")
                    field_id = stable_id(field_mark[0], symbol)
                    claim(field_id, symbol)
                    field_type = member["type"]
                    spelling = field_type.get("desugaredQualType", field_type["qualType"])
                    fields.append({"id": field_id, "name": member["name"], "owner": qualified,
                                   "spelling": spelling})
                records[qualified] = {"name": qualified, "id": identifier, "version": version,
                                      "base": base, "own_fields": fields}
            for child in node.get("inner", []):
                if child.get("kind") in {"CXXRecordDecl", "ClassTemplateDecl"}:
                    visit(child, scope + ((name,) if name else ()), anonymous, templated)
            return
        for child in node.get("inner", []):
            visit(child, scope, anonymous, templated)

    visit(ast)
    if not records:
        raise MetaError("no ROCKS_REFLECT_TYPE definitions found in the translation unit")

    def resolve_type(spelling, owner):
        spelling = spelling.removeprefix("::")
        if spelling == "rocks::Object":
            return spelling
        if spelling in records:
            return spelling
        scope = owner.split("::")[:-1]
        while scope:
            candidate = "::".join(scope + [spelling])
            if candidate in records:
                return candidate
            scope.pop()
        raise MetaError(f"{owner}: base/reference type {spelling!r} is not reflected in this unit")

    for record in records.values():
        record["base"] = resolve_type(record["base"], record["name"])
        for field in record["own_fields"]:
            spelling = field["spelling"]
            reference = None
            if spelling in SCALARS:
                field["kind"] = SCALARS[spelling]
            elif spelling.startswith("rocks::ObjectRef<") and spelling.endswith(">"):
                target = spelling[len("rocks::ObjectRef<"):-1].strip() or "rocks::Object"
                reference = resolve_type(target, record["name"])
                field["kind"] = "ObjectReference"
            else:
                raise MetaError(f"{field['owner']}::{field['name']}: unsupported field type {spelling!r}")
            field["reference"] = reference

    ordered = []
    visiting = set()
    visited = set()

    def order(name):
        if name in visiting:
            raise MetaError(f"{name}: inheritance cycle")
        if name in visited:
            return
        visiting.add(name)
        record = records[name]
        inherited = []
        if record["base"] != "rocks::Object":
            order(record["base"])
            inherited = records[record["base"]]["fields"]
        record["fields"] = sorted(inherited + record["own_fields"], key=lambda field: field["id"])
        if len(record["fields"]) > 4096:
            raise MetaError(f"{name}: inherited field count exceeds 4096")
        visiting.remove(name)
        visited.add(name)
        ordered.append(record)

    for name in sorted(records):
        order(name)
    return records, ordered


def guid(identifier):
    if identifier is None:
        return "Guid{}"
    return f"Guid{{0x{identifier[:16]}ull, 0x{identifier[16:]}ull}}"


def generate(records, ordered, include):
    if any(character in include for character in '\r\n"\\'):
        raise MetaError("--include must use a safe forward-slash header spelling")
    lines = ["// Generated by Rocks MetaCompiler. Do not edit; stable IDs come from source annotations.",
             "#pragma once", "", f'#include "{include}"', "#include <Meta/GeneratedType.h>",
             "#include <Object/GeneratedFieldAccess.h>", "", "namespace rocks {", ""]
    for record in ordered:
        name = "::" + record["name"]
        base = record["base"]
        base_id = None if base == "rocks::Object" else records[base]["id"]
        fields = record["fields"]
        lines += [f"template <> struct GeneratedType<{name}> {{",
                  f"static constexpr TypeId Id() noexcept {{ return {guid(record['id'])}; }}",
                  "static const TypeSchema& Schema() noexcept {"]
        if fields:
            lines.append("static const FieldSchema fields[] = {")
            for field in fields:
                reference = field["reference"]
                target_id = None if reference in (None, "rocks::Object") else records[reference]["id"]
                kind = "ObjectReference" if reference else "Value"
                label = json.dumps(field["name"], ensure_ascii=False)
                lines.append(f"{{{guid(field['id'])}, FieldKind::{kind}, 0, "
                             f"Span<const char>({label}, {len(field['name'].encode('utf-8'))}), {{}}, "
                             f"ValueKind::{field['kind']}, {guid(target_id)}}},")
            lines.append("};")
        schema_fields = f"Span<const FieldSchema>(fields, {len(fields)})" if fields else "Span<const FieldSchema>{}"
        label = json.dumps(record["name"], ensure_ascii=False)
        lines += [f"static const TypeSchema schema{{Id(), {record['version']}, {schema_fields}, "
                  f"Span<const char>({label}, {len(record['name'].encode('utf-8'))}), {guid(base_id)}}};",
                  "return schema;", "}",
                  "static RuntimeTypeOps Ops(ModuleLease module = {}) noexcept {",
                  f"auto ops = MakeRuntimeTypeOps<{name}>(Move(module));"]
        if fields:
            lines.append("static const RuntimeFieldOps fields[] = {")
            for field in fields:
                member = f"&::{field['owner']}::{field['name']}"
                lines += [f"{{{guid(field['id'])},",
                          "+[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, FieldValue& out) noexcept -> Result<void> {",
                          f"return detail::ReadGeneratedField<{name}>(registry, access, object, {member}, out); }} ,",
                          "+[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {",
                          f"return detail::WriteGeneratedField<{name}>(registry, access, object, {member}, value); }} ,",
                          "+[](ConstructionContext& construction, ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {",
                          f"return detail::WriteGeneratedStagedField<{name}>(construction, object, {member}, value); }} }},"]
            lines += ["};", f"ops.fields = Span<const RuntimeFieldOps>(fields, {len(fields)});"]
        if base_id is not None:
            lines.append(f"ops.nativeBaseType = NativeTypeKey<::{base}>();")
        lines += ["ops.completeFieldLayout = true;", "ops.parallelTraceSafe = true;",
                  "ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept {"]
        references = [field for field in fields if field["reference"]]
        if references:
            lines.append(f"const auto& typed = static_cast<const {name}&>(object);")
            for field in references:
                lines.append(f"visitor.Visit(typed.*(&::{field['owner']}::{field['name']}));")
        else:
            lines += ["(void)object;", "(void)visitor;"]
        lines += ["};", "return ops;", "}",
                  "static Result<TypeRegistrationToken> Register(TypeRegistry& registry, ModuleLease module = {}) noexcept {",
                  "return registry.Register(Schema(), Ops(Move(module)));", "}", "};", ""]
    lines += ["} // namespace rocks", ""]
    return "\n".join(lines)


def run(args):
    env = environment()
    compiler = args.compiler or ("clang-cl" if sys.platform == "win32" else "clang++")
    executable = shutil.which(compiler, path=env.get("PATH"))
    if not executable:
        raise MetaError(f"Clang compiler not found: {compiler}")
    source = Path(args.input).resolve()
    if not source.is_file():
        raise MetaError(f"input header not found: {source}")
    if any(character in source.as_posix() for character in '\r\n"'):
        raise MetaError("unsupported input path spelling")
    includes = [str(path) for path in sorted((ROOT / "Engine").glob("*/Public"))]
    includes += [str(ROOT)] + args.include_dir
    if "clang-cl" in Path(executable).stem:
        command = [executable, "/nologo", "/TP", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-",
                   "/Zc:__cplusplus", "/clang:-fsyntax-only", "/clang:-Xclang", "/clang:-ast-dump=json"]
        command += ["/I" + path for path in includes]
    else:
        command = [executable, "-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   "-fno-exceptions", "-fno-rtti", "-fsyntax-only", "-Xclang", "-ast-dump=json"]
        for path in includes:
            command += ["-I", path]
    command += args.clang_arg + ["-"]
    violations = command_errors(" ".join(command))
    if violations or any(argument.startswith("@") for argument in args.clang_arg):
        raise MetaError("unsupported compiler diagnostics/response-file override: " + "; ".join(violations))
    compiled = subprocess.run(command, input=f'#include "{source.as_posix()}"\n',
                              capture_output=True, text=True, encoding="utf-8", errors="strict", env=env)
    if compiled.returncode:
        raise MetaError("Clang rejected reflection input:\n" + compiled.stderr)
    try:
        records, ordered = collect(json.loads(compiled.stdout))
    except (ValueError, KeyError, TypeError) as error:
        raise MetaError(f"unsupported Clang AST shape: {error}") from error
    output = generate(records, ordered, args.include)
    formatter = os.environ.get("ROCKS_CLANG_FORMAT") or shutil.which("clang-format", path=env.get("PATH"))
    if not formatter:
        raise MetaError("clang-format is required for deterministic generated code")
    version = subprocess.check_output([formatter, "--version"], text=True, env=env)
    if "version 21.1.6" not in version:
        raise MetaError("Rocks requires clang-format 21.1.6; set ROCKS_CLANG_FORMAT")
    formatted = subprocess.run([formatter, "--assume-filename=" + str(ROOT / "Engine/Metadata.h")],
                               input=output, capture_output=True, text=True, encoding="utf-8", env=env)
    if formatted.returncode:
        raise MetaError("clang-format failed: " + formatted.stderr)
    encoded = formatted.stdout.replace("\r\n", "\n").encode("utf-8")
    destination = Path(args.output)
    if args.check:
        if not destination.is_file() or destination.read_bytes() != encoded:
            raise MetaError(f"generated output is stale: {destination}")
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.is_file() or destination.read_bytes() != encoded:
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + ".",
                                             suffix=".tmp", delete=False) as file:
                temporary = Path(file.name)
                file.write(encoded)
            try:
                temporary.replace(destination)
            finally:
                temporary.unlink(missing_ok=True)
    print(f"Rocks metadata: {len(ordered)} types; " + ("checked" if args.check else "generated"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True)
    parser.add_argument("--include", required=True, help="logical include spelling embedded in output")
    parser.add_argument("--output", required=True)
    parser.add_argument("--compiler")
    parser.add_argument("--include-dir", action="append", default=[])
    parser.add_argument("--clang-arg", action="append", default=[])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        run(args)
    except (MetaError, OSError, subprocess.SubprocessError) as error:
        print("Rocks MetaCompiler: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
