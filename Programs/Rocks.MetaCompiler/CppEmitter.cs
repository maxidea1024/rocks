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

    // 모듈 집계 헤더다. 의존 모듈(import)의 RegisterModuleTypes를 먼저 부르고 이 모듈 타입을 enum → struct(쓰는
    // struct 먼저 — 순위·이름 순) → 클래스(base 먼저) 순으로 등록한다(같은 ID·같은 native 타입이 이미 있으면
    // 건너뛴다). 실패하면 이 호출이 등록한 것을 역순으로 해제한다. private 집계는 public 집계를(public 헤더가
    // 없으면 의존 모듈 집계를 직접) 먼저 부른 뒤 private 타입을 등록한다. 모듈 이름이 rocks의 이름(Result 등)을
    // 가리지 않도록 모든 이름을 ::rocks::로 한정한다.
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
            if (type.IsEnum)
            {
                Line($"if (auto added = registration.RegisterEnumIfAbsent(::rocks::GeneratedType<::{type.Name}>::Schema(), ::rocks::RuntimeEnumOps{{::rocks::NativeTypeKey<::{type.Name}>(), module}}); !added) return fail(added.Error());");
                continue;
            }

            if (type.IsStruct)
            {
                Line($"if (auto added = registration.RegisterStructIfAbsent(::rocks::GeneratedType<::{type.Name}>::Schema(), ::rocks::GeneratedType<::{type.Name}>::Ops(module)); !added) return fail(added.Error());");
                continue;
            }

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
        Line("#include <Object/GeneratedFieldCodec.h>");
        // 다른 헤더(같은 모듈·다른 모듈)에 선언된 리플렉션 enum·struct를 필드(상속 필드 포함)나 NATIVE 멤버의 직접
        // 타입으로 쓰면 그 타입의 GeneratedType이 먼저 보여야 한다(생성 접근기가 GeneratedType<T>::Id·IsDeclared·
        // ContainsStrongReference로 알아본다). 같은 특수화를 두 번 정의하지 않도록 그 타입의 생성 헤더를 include한다.
        ClassModel[] own = schema.Ordered.Where(type => type.Header == include).ToArray();
        IEnumerable<string> valueHeaders = own.SelectMany(type => type.Fields)
            .Where(field => FieldModel.KindRequiresTargetId(field.Kind) && field.Reference is not null)
            .Select(field => field.Reference!)
            .Concat(own.SelectMany(type => type.NativeMembers).Where(native => native.Struct is not null).Select(native => native.Struct!))
            .Select(name => schema.Find(name).Header)
            .Where(header => header != include)
            .Distinct(StringComparer.Ordinal)
            .Order(StringComparer.Ordinal);
        foreach (string header in valueHeaders)
        {
            Line($"#include <{CompilerOptions.GeneratedSpelling(header)}>");
        }

        if (own.Any(type => type.NativeMembers.Count != 0))
        {
            Line("#include <Object/NativeMemberTraits.h>");
        }

        Line("namespace rocks {");
        // 특수화는 schema.Ordered 순서다: enum, struct(PROPERTY·NATIVE로 쓰는 struct 먼저), 클래스(base 먼저). 같은 헤더 안에서도 쓰는
        // 타입의 특수화가 먼저 나온다.
        foreach (ClassModel type in own)
        {
            if (type.IsEnum)
            {
                EmitEnum(type, Line);
                continue;
            }

            if (type.IsStruct)
            {
                EmitStruct(type, Line);
                continue;
            }

            string native = "::" + type.Name;
            string? baseId = type.Base == "rocks::Object" ? null : schema.Find(type.Base).Id;
            Line($"template <> struct GeneratedType<{native}> {{");
            // ROCKS_NATIVE 멤버는 스키마에 넣지 않는다. MetaCompiler의 AST 재귀 검사(RMC2205·2206)를 직접 타입에
            // 대해 컴파일러가 한 번 더 확인한다(GeneratedType은 GENERATED_BODY로 private 멤버에도 접근한다).
            foreach (NativeMemberModel member in type.NativeMembers)
            {
                string memberType = $"decltype({native}::{member.Name})";
                Line($"static_assert(!IsNativeStrongReference<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} must not hold a strong object reference (RMC2205)\");");
                if (!type.WorldOnly)
                {
                    Line($"static_assert(!RequiresWorldOnlyOwner<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner (RMC2206)\");");
                }

                EmitNativeStructAsserts(type, member, !type.WorldOnly, Line);
            }

            // WorldOnly가 아닌 클래스는 struct 필드(상속 포함)의 생성 형질을 직접 확인한다(import flags를 믿지 않는다).
            if (!type.WorldOnly)
            {
                foreach (string structure in type.Fields.Where(field => field.IsStruct).Select(field => field.Reference!).Distinct(StringComparer.Ordinal))
                {
                    Line($"static_assert(!GeneratedType<::{structure}>::RequiresWorldOnlyOwner, \"struct {structure} requires a WorldOnly owner; {type.Name} is not WorldOnly (RMC2206)\");");
                }
            }

            Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
            Line("static const TypeSchema& Schema() noexcept {");
            EmitFieldSchemas(type, Line);
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
                    if (field.IsStruct)
                    {
                        // struct 필드는 FieldValue로 표현할 수 없으므로 read·write·writeStaged가 없고(쓰기는 Unsupported —
                        // 멤버 단위 쓰기는 B6 경로 접근) codec과 struct native 키만 싣는다(Docs/Design/
                        // ReflectionFoundations.ko.md 4.6절).
                        Line("nullptr, {}, nullptr,");
                        EmitClassCodec(native, member, $", NativeTypeKey<::{field.Reference}>()", Line);
                        continue;
                    }

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
                    Line($"return detail::WriteGeneratedStagedField<{native}>(construction, object, {member}, value); }},");
                    // Enum 필드는 정적 thunk의 enum native 키를 싣는다(등록이 value.type의 enum 레코드와 대조한다).
                    EmitClassCodec(native, member, field.Kind == "Enum" ? $", NativeTypeKey<::{field.Reference}>()" : "", Line);
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
            FieldModel[] traced = type.Fields.Where(field => field.IsReference || field.IsStruct).ToArray();
            if (traced.Length != 0)
            {
                Line($"const auto& typed = static_cast<const {native}&>(object);");
                // Transient는 영속성 정책이며 strong edge의 GC 도달성에는 영향을 주지 않는다. struct 필드는 모든 struct가
                // 내는 Trace를 부른다(참조 없는 struct는 빈 본문 — 이 람다는 비템플릿이라 if constexpr로 가를 수 없다).
                foreach (FieldModel field in traced)
                {
                    Line(field.IsStruct
                        ? $"GeneratedType<::{field.Reference}>::Trace(typed.*(&::{field.Owner}::{field.Name}), visitor);"
                        : $"visitor.Visit(typed.*(&::{field.Owner}::{field.Name}));");
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

    // 클래스 필드의 그래프 저장·복원 codec 두 thunk와 함수표 끝이다(Docs/Design/ReflectionFoundations.ko.md 6.4절).
    // 문맥이 참조 wire·진단·예산을 맡으므로 그래프 형식이 바뀌어도 이 코드를 다시 만들지 않는다. valueNativeType은
    // Enum·Struct 필드가 싣는 ", NativeTypeKey<T>()"(그 밖은 "")다.
    private static void EmitClassCodec(string native, string member, string valueNativeType, Action<string> Line)
    {
        Line("+[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object, ArchiveWriter& payload, FieldEncodeContext& context) noexcept -> Result<uint32> {");
        Line($"return detail::EncodeGeneratedField<{native}>(registry, access, object, {member}, payload, context); }},");
        Line("+[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object, const FieldView& field, FieldDecodeContext& context) noexcept -> Result<void> {");
        Line($"return detail::DecodeGeneratedField<{native}>(construction, object, {member}, field, context); }}{valueNativeType} }},");
    }

    // NATIVE 멤버의 직접 타입이 리플렉션 struct면 그 struct의 생성 형질로 RMC2205(·worldOnlyCheck면 RMC2206)를
    // 컴파일 시 한 번 더 확인한다(NativeMemberTraits는 직접 타입의 형질만 본다 — GeneratedType 탐지 특수화를 두면
    // 생성 헤더가 없을 때 조용히 거짓이 되는 ODR 위반이라 생성 코드가 확인한다).
    private static void EmitNativeStructAsserts(ClassModel type, NativeMemberModel member, bool worldOnlyCheck, Action<string> Line)
    {
        if (member.Struct is null)
        {
            return;
        }

        Line($"static_assert(!GeneratedType<::{member.Struct}>::ContainsStrongReference, \"ROCKS_NATIVE member {type.Name}::{member.Name} holds reference struct {member.Struct} (RMC2205)\");");
        if (worldOnlyCheck)
        {
            Line($"static_assert(!GeneratedType<::{member.Struct}>::RequiresWorldOnlyOwner, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner (RMC2206)\");");
        }
    }

    // 리플렉션 struct의 GeneratedType 특수화다(Docs/Design/ReflectionFoundations.ko.md 4.6절): 형질
    // (ContainsStrongReference·RequiresWorldOnlyOwner)과 중첩 깊이, Id, Schema(base·클래스 플래그 없는 TypeSchema),
    // Trace(모든 struct — 참조 없는 struct는 빈 본문), Ops(RuntimeStructOps — 멤버 codec 함수표, 참조 struct만 trace,
    // Migrate가 있으면 migrate), Register. 형질과 깊이는 모델 값을 내되 직접 멤버의 생성 값을 포함하는지
    // static_assert로 대조한다(import descriptor의 flags를 믿지 않는다).
    private static void EmitStruct(ClassModel type, Action<string> Line)
    {
        string native = "::" + type.Name;
        string True(bool value) => value ? "true" : "false";
        Line($"template <> struct GeneratedType<{native}> {{");
        Line($"static constexpr bool ContainsStrongReference = {True(type.ContainsStrongReference)};");
        Line($"static constexpr bool RequiresWorldOnlyOwner = {True(type.RequiresWorldOnlyOwner)};");
        Line($"static constexpr uint32 Depth = {type.Depth}u;");
        Line($"static_assert(Depth <= ::rocks::MaxStructDepth, \"ROCKS_STRUCT {type.Name} nesting depth exceeds MaxStructDepth (RMC2303)\");");
        foreach (string structure in type.Fields.Where(field => field.IsStruct).Select(field => field.Reference!).Distinct(StringComparer.Ordinal))
        {
            string member = $"GeneratedType<::{structure}>";
            Line($"static_assert({member}::Depth < Depth, \"ROCKS_STRUCT {type.Name} nests {structure} deeper than recorded; regenerate metadata (RMC2303)\");");
            Line($"static_assert(ContainsStrongReference || !{member}::ContainsStrongReference, \"ROCKS_STRUCT {type.Name} holds reference struct {structure}; regenerate metadata\");");
            Line($"static_assert(RequiresWorldOnlyOwner || !{member}::RequiresWorldOnlyOwner, \"ROCKS_STRUCT {type.Name} holds struct {structure} that requires a WorldOnly owner; regenerate metadata\");");
        }

        // NATIVE 멤버는 스키마에 넣지 않는다. 강한 참조 금지(RMC2205)를 직접 타입에 대해 한 번 더 확인하고, WorldOnly
        // 소유자 형질은 struct에서는 오류가 아니라 RequiresWorldOnlyOwner로 전파되었는지 확인한다.
        foreach (NativeMemberModel member in type.NativeMembers)
        {
            string memberType = $"decltype({native}::{member.Name})";
            Line($"static_assert(!IsNativeStrongReference<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} must not hold a strong object reference (RMC2205)\");");
            Line($"static_assert(RequiresWorldOnlyOwner || !::rocks::RequiresWorldOnlyOwner<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner; regenerate metadata\");");
            EmitNativeStructAsserts(type, member, false, Line);
            if (member.Struct is not null)
            {
                Line($"static_assert(RequiresWorldOnlyOwner || !GeneratedType<::{member.Struct}>::RequiresWorldOnlyOwner, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner; regenerate metadata\");");
            }
        }

        Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
        Line("static const TypeSchema& Schema() noexcept {");
        EmitFieldSchemas(type, Line);
        string fieldSpan = type.Fields.Count == 0 ? "Span<const FieldSchema>{}" : $"Span<const FieldSchema>(fields, {type.Fields.Count})";
        Line($"static const TypeSchema schema{{Id(), {type.Version}, {fieldSpan}, {View(type.Name)}, Guid{{}}, 0u}};");
        Line("return schema;");
        Line("}");
        // 참조 struct는 직접 ObjectRef 멤버(Transient 포함 — GC는 추적한다)를 Visit하고 멤버 struct의 Trace를 부른다.
        FieldModel[] traced = type.ContainsStrongReference ? type.Fields.Where(field => field.IsReference || field.IsStruct).ToArray() : [];
        if (traced.Length == 0)
        {
            Line($"static void Trace(const {native}&, TraceVisitor&) noexcept {{}}");
        }
        else
        {
            Line($"static void Trace(const {native}& value, TraceVisitor& visitor) noexcept {{");
            foreach (FieldModel field in traced)
            {
                Line(field.IsStruct
                    ? $"GeneratedType<::{field.Reference}>::Trace(value.*(&{native}::{field.Name}), visitor);"
                    : $"visitor.Visit(value.*(&{native}::{field.Name}));");
            }

            Line("}");
        }

        Line("static RuntimeStructOps Ops(ModuleLease module = {}) noexcept {");
        Line($"auto ops = MakeRuntimeStructOps<{native}>(Move(module));");
        if (type.Fields.Count != 0)
        {
            Line("static const RuntimeStructFieldOps fields[] = {");
            foreach (FieldModel field in type.Fields)
            {
                Line($"detail::StructFieldOps<&{native}::{field.Name}>({GuidValue(field.Id)}),");
            }

            Line("};");
            Line($"ops.fields = Span<const RuntimeStructFieldOps>(fields, {type.Fields.Count});");
        }

        if (type.ContainsStrongReference)
        {
            Line($"ops.trace = +[](const void* value, TraceVisitor& visitor) noexcept {{ Trace(*static_cast<const {native}*>(value), visitor); }};");
        }

        // struct가 선언한 Migrate를 타입 마이그레이션으로 연결한다. 시그니처·noexcept가 StructMigration과 다르거나
        // 접근할 수 없으면(private인데 GENERATED_BODY 없음) 컴파일 오류다 — 조용히 마이그레이션 없이 등록되지 않는다.
        if (type.HasMigrate)
        {
            Line($"ops.migrate = &{native}::Migrate;");
        }

        Line("ops.requiresWorldOnlyOwner = RequiresWorldOnlyOwner;");
        Line("return ops;");
        Line("}");
        Line("static Result<TypeRegistrationToken> Register(TypeRegistry& registry, ModuleLease module = {}) noexcept {");
        Line("return registry.RegisterStruct(Schema(), Ops(Move(module)));");
        Line("}");
        Line("};");
    }

    // 클래스·struct 스키마의 필드 배열이다(필드가 없으면 내지 않는다).
    private static void EmitFieldSchemas(ClassModel type, Action<string> Line)
    {
        if (type.Fields.Count == 0)
        {
            return;
        }

        Line("static const FieldSchema fields[] = {");
        foreach (FieldModel field in type.Fields)
        {
            string? targetId = field.HasTargetType ? field.ReferenceId : null;
            string kind = field.IsReference ? "ObjectReference" : "Value";
            Line($"{{{GuidValue(field.Id)}, FieldKind::{kind}, {field.Flags}u, {View(field.Name)}, {{}}, {{ValueKind::{field.Kind}, {GuidValue(targetId)}}}, {View(field.Spec.Category)}, {View(field.Spec.DisplayName)}, {View(field.Spec.ToolName)}}},");
        }

        Line("};");
    }

    // 리플렉션 enum의 GeneratedType 특수화다: Id, Schema(EnumSchema), constexpr IsDeclared(쓰기·복원 검증),
    // Register. C++ underlying이 스키마와 같은지 컴파일 시 확인한다.
    private static void EmitEnum(ClassModel type, Action<string> Line)
    {
        string native = "::" + type.Name;
        Line($"template <> struct GeneratedType<{native}> {{");
        Line($"static_assert(__is_same(__underlying_type({native}), {EnumRules.NativeType(type.Underlying)}), \"ROCKS_ENUM {type.Name} underlying type changed; regenerate metadata\");");
        // 스키마의 값이 C++ 값과 같은지 컴파일 시 대조한다(MetaCompiler가 AST에서 읽은 값을 믿지 않는다).
        foreach (EnumeratorModel value in type.Enumerators)
        {
            Line($"static_assert(static_cast<int64>({native}::{value.Name}) == {Int64(value.Value)}, \"ROCKS_ENUM {type.Name}::{value.Name} value changed; regenerate metadata\");");
        }

        Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
        Line("static const EnumSchema& Schema() noexcept {");
        Line("static const EnumeratorSchema values[] = {");
        foreach (EnumeratorModel value in type.Enumerators)
        {
            Line($"{{{View(value.Name)}, {View(value.ToolName)}, {View(value.DisplayName)}, {Int64(value.Value)}, {(value.Hidden ? 1 : 0)}u}},");
        }

        Line("};");
        Line($"static const EnumSchema schema{{Id(), {View(type.Name)}, ValueKind::{type.Underlying}, {(type.FlagsEnum ? 1 : 0)}u, Span<const EnumeratorSchema>(values, {type.Enumerators.Count}), {View(type.ToolPrefix)}}};");
        Line("return schema;");
        Line("}");
        // TypeRegistry 없이도(staged 쓰기·복원) 같은 규칙으로 값을 검증한다(IsDeclaredEnumValue와 같다).
        Line("static constexpr bool IsDeclared(int64 value) noexcept {");
        if (type.FlagsEnum)
        {
            ulong mask = type.Enumerators.Aggregate(0UL, (bits, value) => bits | (ulong)value.Value);
            long maximum = EnumRules.Range(type.Underlying)!.Value.Maximum;
            Line($"return value >= 0 && value <= {Int64(maximum)} && (static_cast<uint64>(value) & ~0x{mask:x}ull) == 0;");
        }
        else
        {
            Line("switch (value) {");
            foreach (EnumeratorModel value in type.Enumerators)
            {
                Line($"case {Int64(value.Value)}:");
            }

            Line("return true;");
            Line("default:");
            Line("return false;");
            Line("}");
        }

        Line("}");
        Line("static Result<TypeRegistrationToken> Register(TypeRegistry& registry, ModuleLease module = {}) noexcept {");
        Line($"return registry.RegisterEnum(Schema(), RuntimeEnumOps{{NativeTypeKey<{native}>(), Move(module)}});");
        Line("}");
        Line("};");
    }

    // int64 리터럴이다. 최솟값은 리터럴로 쓸 수 없지만 int32 범위의 enum만 있으므로 그 경우가 없다.
    private static string Int64(long value) => value.ToString(CultureInfo.InvariantCulture) + "ll";
}
