using System.Globalization;
using System.Text;

namespace Rocks.MetaCompiler;

internal static class CppEmitter
{
    private static string GuidValue(string? id) => id is null ? "Guid{}" : $"Guid{{0x{id[..16]}ull, 0x{id[16..]}ull}}";

    private static readonly UTF8Encoding Utf8 = new(false, true);

    // 읽을 수 있는 UTF-8은 그대로 남긴다. 제어 바이트는 정확히 세 자리 8진수로 출력하여
    // 뒤 문자가 숫자·16진수여도 escape에 흡수되지 않게 한다. Span 길이는 항상 UTF-8 바이트다.
    private static string Label(string text)
    {
        _ = Utf8.GetBytes(text); // 잘못된 surrogate를 대체 문자로 바꾸지 않고 거부한다.
        StringBuilder result = new("\"");
        foreach (Rune rune in text.EnumerateRunes())
        {
            if (rune.Value == '"')
            {
                result.Append("\\\"");
            }
            else if (rune.Value == '\\')
            {
                result.Append("\\\\");
            }
            else if (rune.Value == '\n')
            {
                result.Append("\\n");
            }
            else if (rune.Value == '\r')
            {
                result.Append("\\r");
            }
            else if (rune.Value == '\t')
            {
                result.Append("\\t");
            }
            else if (Rune.GetUnicodeCategory(rune) is UnicodeCategory.Control or UnicodeCategory.Format or
                     UnicodeCategory.LineSeparator or UnicodeCategory.ParagraphSeparator)
            {
                foreach (byte value in Utf8.GetBytes(rune.ToString()))
                {
                    result.Append('\\').Append(Convert.ToString(value, 8).PadLeft(3, '0'));
                }
            }
            else
            {
                result.Append(rune.ToString());
            }
        }

        return result.Append('"').ToString();
    }

    private static string View(string text) => $"Span<const char>({Label(text)}, {Utf8.GetByteCount(text)})";

    // 모듈 집계 헤더다. 의존 모듈(import)의 RegisterModuleTypes를 먼저 부르고 이 모듈 타입을 base 먼저
    // 등록한다(같은 ID·같은 native 타입이 이미 있으면 건너뛴다). 실패하면 이 호출이 등록한 것을 역순으로
    // 해제한다. private 집계는 public 집계를(public 헤더가 없으면 의존 모듈 집계를 직접) 먼저 부른 뒤 private
    // 타입을 등록한다. 모듈 이름이 rocks의 이름(Result 등)을 가리지 않도록 모든 이름을 ::rocks::로 한정한다.
    public static string EmitAggregate(string module, SchemaModel schema, IReadOnlyList<ModuleInput> inputs,
        IReadOnlyList<string> importedModules, bool privateTypes)
    {
        StringBuilder output = new();
        void Line(string text = "") => output.Append(text).Append('\n');
        Line("// Rocks MetaCompiler 생성 파일이다. 직접 편집하지 않는다. 모듈의 리플렉션 타입을 한 번에 등록한다.");
        Line("#pragma once");
        bool viaPublic = privateTypes && inputs.Any(input => !input.Private);
        string[] dependencies = viaPublic ? [] : importedModules.Order(StringComparer.Ordinal).ToArray();
        if (viaPublic)
        {
            Line($"#include <{CompilerOptions.AggregateSpelling(module, false)}>");
        }

        foreach (string dependency in dependencies)
        {
            Line($"#include <{CompilerOptions.AggregateSpelling(dependency, false)}>");
        }

        foreach (ModuleInput input in inputs.Where(input => input.Private == privateTypes).OrderBy(input => input.Include, StringComparer.Ordinal))
        {
            Line($"#include <{CompilerOptions.GeneratedSpelling(input.Include)}>");
        }

        Line("#include <Object/ModuleRegistration.h>");
        Line($"namespace rocks::generated::{module} {{");
        string function = privateTypes ? "RegisterPrivateModuleTypes" : "RegisterModuleTypes";
        Line($"inline ::rocks::Result<void> {function}(::rocks::ModuleRegistration& registration, const ::rocks::ModuleLease& module = {{}}) noexcept {{");
        // 의존 그래프의 여러 경로로 다시 불려도 registration마다 모듈을 한 번만 등록한다(호출이 경로 수만큼 늘지 않게).
        // 인라인 함수의 정적 지역 객체는 프로그램 전체에서 하나이므로 그 주소가 모듈 키다.
        Line("static constexpr char moduleKey = 0;");
        Line("const ::rocks::usize mark = registration.Size();");
        Line("auto entered = registration.EnterModule(&moduleKey);");
        Line("if (!entered) return ::rocks::Result<void>::Failure(entered.Error());");
        Line("if (!entered.Value()) return ::rocks::Result<void>::Success();");
        Line("auto fail = [&](::rocks::ErrorCode error) noexcept { static_cast<void>(registration.UnregisterFrom(mark)); return ::rocks::Result<void>::Failure(error); };");
        if (viaPublic)
        {
            Line($"if (auto dependencies = ::rocks::generated::{module}::RegisterModuleTypes(registration, module); !dependencies) return fail(dependencies.Error());");
        }

        foreach (string dependency in dependencies)
        {
            Line($"if (auto dependency = ::rocks::generated::{dependency}::RegisterModuleTypes(registration, module); !dependency) return fail(dependency.Error());");
        }

        foreach (ClassModel type in schema.Ordered.Where(type => type.Private == privateTypes))
        {
            Line($"if (auto added = registration.RegisterIfAbsent(::rocks::GeneratedType<::{type.Name}>::Schema(), ::rocks::GeneratedType<::{type.Name}>::Ops(module)); !added) return fail(added.Error());");
        }

        Line("(void)fail;");
        Line("return ::rocks::Result<void>::Success();");
        Line("}");
        Line($"}} // namespace rocks::generated::{module}");
        return output.ToString();
    }

    // 입력 헤더 하나에 선언된 타입의 GeneratedType 특수화를 낸다.
    public static string Emit(SchemaModel schema, ModuleInput input)
    {
        string include = input.Include;
        if (include.Length == 0 || include.IndexOfAny(['\r', '\n', '"', '\\', '<', '>']) >= 0)
        {
            throw new MetadataException("--include must use a safe forward-slash header spelling", "RMC1000",
                "Pass the logical generated include with forward slashes and no quotes or line breaks.");
        }

        StringBuilder output = new();
        void Line(string text = "") => output.Append(text).Append('\n');
        Line("// Rocks MetaCompiler 생성 파일이다. 직접 편집하지 않는다. 안정 ID는 원본 annotation에서 유지한다.");
        Line("#pragma once");
        Line($"#include <{include}>");
        Line("#include <Meta/GeneratedType.h>");
        Line("#include <Object/GeneratedFieldAccess.h>");
        if (schema.Ordered.Any(type => type.Header == include && type.NativeMembers.Count != 0))
        {
            Line("#include <Object/NativeMemberTraits.h>");
        }

        Line("namespace rocks {");
        foreach (ClassModel type in schema.Ordered.Where(type => type.Header == include))
        {
            string native = "::" + type.Name;
            string? baseId = type.Base == "rocks::Object" ? null : schema.Find(type.Base).Id;
            Line($"template <> struct GeneratedType<{native}> {{");
            // ROCKS_NATIVE 멤버는 스키마에 넣지 않는다. MetaCompiler의 AST 재귀 검사(RMC2205·2206)를 직접 타입에
            // 대해 컴파일러가 한 번 더 확인한다(GeneratedType은 GENERATED_BODY로 private 멤버에도 접근한다).
            foreach (string member in type.NativeMembers)
            {
                string memberType = $"decltype({native}::{member})";
                Line($"static_assert(!IsNativeStrongReference<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member} must not hold a strong object reference (RMC2205)\");");
                if (!type.WorldOnly)
                {
                    Line($"static_assert(!RequiresWorldOnlyOwner<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member} requires a WorldOnly owner (RMC2206)\");");
                }
            }

            Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
            Line("static const TypeSchema& Schema() noexcept {");
            if (type.Fields.Count != 0)
            {
                Line("static const FieldSchema fields[] = {");
                foreach (FieldModel field in type.Fields)
                {
                    string? targetId = field.HasTargetType ? field.ReferenceId : null;
                    string kind = field.IsReference ? "ObjectReference" : "Value";
                    Line($"{{{GuidValue(field.Id)}, FieldKind::{kind}, {field.Flags}u, {View(field.Name)}, {{}}, {{ValueKind::{field.Kind}, {GuidValue(targetId)}}}, {View(field.Spec.Category)}, {View(field.Spec.DisplayName)}, {View(field.Spec.ToolName)}}},");
                }

                Line("};");
            }

            string fieldSpan = type.Fields.Count == 0 ? "Span<const FieldSchema>{}" : $"Span<const FieldSchema>(fields, {type.Fields.Count})";
            Line($"static const TypeSchema schema{{Id(), {type.Version}, {fieldSpan}, {View(type.Name)}, {GuidValue(baseId)}, {type.Flags}u}};");
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
                    else
                    {
                        Line($"return detail::WriteGeneratedField<{native}>(registry, access, object, {member}, value);");
                    }

                    Line("},");
                    Line("+[](ConstructionContext& construction, ObjectHandle object, const FieldValue& value) noexcept -> Result<void> {");
                    Line($"return detail::WriteGeneratedStagedField<{native}>(construction, object, {member}, value); }} }},");
                }

                Line("};");
                Line($"ops.fields = Span<const RuntimeFieldOps>(fields, {type.Fields.Count});");
            }

            if (baseId is not null)
            {
                // nativeBaseType은 상속 Resolve의 포인터 변환 근거다. 오래된 생성 헤더나 잘못된 base 선언이
                // 조용히 컴파일되지 않도록 C++ 상속을 컴파일 시 확인한다.
                Line($"static_assert(__is_base_of(::{type.Base}, {native}), \"reflected base must be a C++ base\");");
                Line($"ops.nativeBaseType = NativeTypeKey<::{type.Base}>();");
            }

            Line("ops.completeFieldLayout = true;");
            Line("ops.parallelTraceSafe = true;");
            Line("ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept {");
            FieldModel[] references = type.Fields.Where(field => field.IsReference).ToArray();
            if (references.Length != 0)
            {
                Line($"const auto& typed = static_cast<const {native}&>(object);");
                // Transient는 영속성 정책이며 strong edge의 GC 도달성에는 영향을 주지 않는다.
                foreach (FieldModel field in references)
                {
                    Line($"visitor.Visit(typed.*(&::{field.Owner}::{field.Name}));");
                }
            }
            else
            {
                Line("(void)object;");
                Line("(void)visitor;");
            }

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
