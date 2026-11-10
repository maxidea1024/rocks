using System.Text;

namespace Rocks.MetaCompiler;

internal static class CppEmitter
{
    private static string GuidValue(string? id) => id is null ? "Guid{}" : $"Guid{{0x{id[..16]}ull, 0x{id[16..]}ull}}";
    // 모든 label을 UTF-8 바이트 escape로 출력하여 host 실행 문자셋에 의존하지 않는다.
    private static string Label(string text) => "\"" + string.Concat(Encoding.UTF8.GetBytes(text).Select(value => $"\\x{value:X2}")) + "\"";
    private static string View(string text) => $"Span<const char>({Label(text)}, {Encoding.UTF8.GetByteCount(text)})";

    public static string Emit(SchemaModel schema, string include)
    {
        if (include.Length == 0 || include.IndexOfAny(['\r', '\n', '"', '\\']) >= 0)
            throw new MetadataException("--include must use a safe forward-slash header spelling", "RMC1000",
                "Pass the logical generated include with forward slashes and no quotes or line breaks.");
        StringBuilder output = new();
        void Line(string text = "") => output.Append(text).Append('\n');
        Line("// Rocks MetaCompiler 생성 파일이다. 직접 편집하지 않는다. 안정 ID는 원본 annotation에서 유지한다.");
        Line("#pragma once");
        Line($"#include \"{include}\"");
        Line("#include <Meta/GeneratedType.h>");
        Line("#include <Object/GeneratedFieldAccess.h>");
        Line("namespace rocks {");
        foreach (ClassModel type in schema.Ordered)
        {
            string native = "::" + type.Name;
            string? baseId = type.Base == "rocks::Object" ? null : schema.Classes[type.Base].Id;
            Line($"template <> struct GeneratedType<{native}> {{");
            Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
            Line("static const TypeSchema& Schema() noexcept {");
            if (type.Fields.Count != 0)
            {
                Line("static const FieldSchema fields[] = {");
                foreach (FieldModel field in type.Fields)
                {
                    string? targetId = field.Reference is null or "rocks::Object" ? null : schema.Classes[field.Reference].Id;
                    string kind = field.Reference is null ? "Value" : "ObjectReference";
                    Line($"{{{GuidValue(field.Id)}, FieldKind::{kind}, {field.Flags}u, {View(field.Name)}, {{}}, ValueKind::{field.Kind}, {GuidValue(targetId)}, {View(field.Spec.Category)}, {View(field.Spec.DisplayName)}}},");
                }
                Line("};");
            }
            string fieldSpan = type.Fields.Count == 0 ? "Span<const FieldSchema>{}" : $"Span<const FieldSchema>(fields, {type.Fields.Count})";
            Line($"static const TypeSchema schema{{Id(), {type.Version}, {fieldSpan}, {View(type.Name)}, {GuidValue(baseId)}, {(type.Abstract ? "1u" : "0u")}}};");
            Line("return schema;");
            Line("}");
            Line("static RuntimeTypeOps Ops(ModuleLease module = {}) noexcept {");
            Line($"auto ops = MakeRuntimeTypeOps<{native}, {(type.Abstract ? "true" : "false")}>(Move(module));");
            if (type.Fields.Count != 0)
            {
                Line("static const RuntimeFieldOps fields[] = {");
                foreach (FieldModel field in type.Fields)
                {
                    string member = $"&::{field.Owner}::{field.Name}";
                    Line($"{{{GuidValue(field.Id)},");
                    Line("+[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, FieldValue& out) noexcept -> Result<void> {");
                    Line($"return detail::ReadGeneratedField<{native}>(registry, access, object, {member}, out); }},");
                    Line("+[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {");
                    if (field.Spec.ReadOnly)
                    {
                        Line("(void)value;");
                        Line($"auto borrowed = registry.Resolve<{native}>(access, object);");
                        Line("if (!borrowed) return Result<void>::Failure(borrowed.Error());");
                        Line("auto valid = borrowed.Value().Get();");
                        Line("if (!valid) return Result<void>::Failure(valid.Error());");
                        Line("return Result<void>::Failure(ErrorCode::Unsupported);");
                    }
                    else Line($"return detail::WriteGeneratedField<{native}>(registry, access, object, {member}, value);");
                    Line("},");
                    Line("+[](ConstructionContext& construction, ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {");
                    Line($"return detail::WriteGeneratedStagedField<{native}>(construction, object, {member}, value); }} }},");
                }
                Line("};");
                Line($"ops.fields = Span<const RuntimeFieldOps>(fields, {type.Fields.Count});");
            }
            if (baseId is not null) Line($"ops.nativeBaseType = NativeTypeKey<::{type.Base}>();");
            Line("ops.completeFieldLayout = true;");
            Line("ops.parallelTraceSafe = true;");
            Line("ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept {");
            FieldModel[] references = type.Fields.Where(field => field.Reference is not null).ToArray();
            if (references.Length != 0)
            {
                Line($"const auto& typed = static_cast<const {native}&>(object);");
                // Transient는 영속성 정책이며 strong edge의 GC 도달성에는 영향을 주지 않는다.
                foreach (FieldModel field in references) Line($"visitor.Visit(typed.*(&::{field.Owner}::{field.Name}));");
            }
            else { Line("(void)object;"); Line("(void)visitor;"); }
            Line("};");
            Line("return ops;");
            Line("}");
            Line("static Result<TypeRegistrationToken> Register(TypeRegistry& registry, ModuleLease module = {}) noexcept {");
            Line("return registry.Register(Schema(), Ops(Move(module)));");
            Line("}");
            Line("};");
        }
        Line("} // namespace rocks");
        return output.ToString();
    }
}
