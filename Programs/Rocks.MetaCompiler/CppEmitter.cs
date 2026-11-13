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
                // Binds 미러는 실제 타입의 GeneratedType이다(NativeName).
                Line($"if (auto added = registration.RegisterStructIfAbsent(::rocks::GeneratedType<::{type.NativeName}>::Schema(), ::rocks::GeneratedType<::{type.NativeName}>::Ops(module)); !added) return fail(added.Error());");
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
        // Binds 미러의 순서 대조(ROCKS_OFFSET_OF)는 Base 매크로를 쓴다(__builtin은 Base 헤더 밖에서 쓰지 않는다).
        if (schema.Ordered.Any(type => type.Header == include && type.Binds.Length != 0))
        {
            Line("#include <Base/Compiler.h>");
        }
        // 다른 헤더(같은 모듈·다른 모듈)에 선언된 리플렉션 enum·struct를 필드(상속 필드·배열 원소 포함)나 NATIVE 멤버의
        // 직접 타입으로 쓰면 그 타입의 GeneratedType이 먼저 보여야 한다(생성 접근기가 GeneratedType<T>::Id·IsDeclared·
        // ContainsStrongReference로 알아보고, StructRefArray<S>::Trace가 GeneratedType<S>::Trace를 인스턴스화한다). 같은
        // 특수화를 두 번 정의하지 않도록 그 타입의 생성 헤더를 include한다.
        ClassModel[] own = schema.Ordered.Where(type => type.Header == include).ToArray();
        IEnumerable<string> valueHeaders = own.SelectMany(type => type.Fields)
            .Where(field => FieldModel.KindRequiresTargetId(field.TargetKind) && field.Reference is not null)
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

        // NATIVE 멤버 static_assert와 struct 특수화의 멤버 형질 대조(IsNativeStrongReference·RegistryContainer)가 쓴다.
        if (own.Any(type => type.NativeMembers.Count != 0 || type.IsStruct))
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
                EmitStruct(schema, type, Line);
                continue;
            }

            string native = "::" + type.NativeName;
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

                EmitNativeStructAsserts(schema, type, member, !type.WorldOnly, Line);
            }

            // WorldOnly가 아닌 클래스는 struct 필드·struct 원소 배열(상속 포함)의 생성 형질을 직접 확인한다(import flags를
            // 믿지 않는다).
            if (!type.WorldOnly)
            {
                foreach (string structure in type.Fields.Where(field => field.StructTarget).Select(field => field.Reference!).Distinct(StringComparer.Ordinal))
                {
                    Line($"static_assert(!GeneratedType<::{schema.Find(structure).NativeName}>::RequiresWorldOnlyOwner, \"struct {structure} requires a WorldOnly owner; {type.Name} is not WorldOnly (RMC2206)\");");
                }
            }

            EmitElementStructAsserts(schema, type, Line);

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
                    if (field.IsStruct || field.IsArray)
                    {
                        // struct·배열 필드는 FieldValue로 표현할 수 없으므로 read·write·writeStaged가 없고(통째 쓰기는
                        // Unsupported — 멤버·원소 단위 쓰기는 B6 경로 접근) codec과 struct native 키(배열이면 원소 Enum·
                        // Struct의 native 키, 그 밖은 없음)만 싣는다(Docs/Design/ReflectionFoundations.ko.md 4.6·5.6절).
                        Line("nullptr, {}, nullptr,");
                        EmitClassCodec(native, member, ValueNativeType(schema, field), Line);
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
            FieldModel[] traced = type.Fields.Where(field => field.IsReference || field.IsStruct || field.IsArray).ToArray();
            if (traced.Length != 0)
            {
                Line($"const auto& typed = static_cast<const {native}&>(object);");
                // Transient는 영속성 정책이며 strong edge의 GC 도달성에는 영향을 주지 않는다. struct 필드는 모든 struct가
                // 내는 Trace를 부른다(참조 없는 struct는 빈 본문 — 이 람다는 비템플릿이라 if constexpr로 가를 수 없다).
                // 배열 필드(상속한 import 필드 포함)는 모두 TraceArrayField를 부른다 — 실제 C++ 타입이 오버로드를 고르므로
                // 컨테이너를 descriptor에서 잘못 추론해도 Trace가 빠지지 않는다.
                foreach (FieldModel field in traced)
                {
                    Line(TraceStatement(schema, field, "typed", $"::{field.Owner}"));
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

    // struct 필드·배열 필드 함수표의 valueNativeType 인자다: struct 필드는 그 struct, 배열은 원소가 Enum·Struct면 그
    // 원소의 native 키(Binds 미러는 실제 타입), 그 밖은 ""(인자 생략 — nullptr)다.
    private static string ValueNativeType(SchemaModel schema, FieldModel field) => field.TargetKind switch
    {
        "Struct" => $", NativeTypeKey<::{schema.Find(field.Reference!).NativeName}>()",
        "Enum" => $", NativeTypeKey<::{field.Reference}>()",
        _ => ""
    };

    // 클래스·참조 struct Trace의 필드 한 줄이다(owner는 typed·value, ownerType은 멤버 포인터의 클래스 철자). ObjectRef는
    // Visit, struct 필드는 그 struct의 생성 Trace, 배열은 TraceArrayField(Array<E>는 아무것도 하지 않고 ObjectRefArray·
    // StructRefArray는 원소를 방문한다)다.
    private static string TraceStatement(SchemaModel schema, FieldModel field, string owner, string ownerType)
    {
        string member = $"{owner}.*(&{ownerType}::{field.Name})";
        if (field.IsArray)
        {
            return $"detail::TraceArrayField({member}, visitor);";
        }

        return field.IsStruct
            ? $"GeneratedType<::{schema.Find(field.Reference!).NativeName}>::Trace({member}, visitor);"
            : $"visitor.Visit({member});";
    }

    // struct 원소 배열 필드(상속 포함)의 원소 struct 형질을 컨테이너와 대조한다(Docs/Design/ReflectionFoundations.ko.md
    // 5.6절): Array<S>는 S가 강한 참조를 담지 않고, StructRefArray<S>는 S가 참조 struct이며 Registry 컨테이너를 담지
    // 않는다. 컨테이너는 원소 종류와 원소 struct 형질로 정해지는 파생 컨테이너다(이 모듈 필드는 C++ 철자와 같다 —
    // 다르면 RMC2304). 생성 static_assert가 import flags를 믿지 않고 원소 struct의 생성 형질로 다시 확인한다.
    private static void EmitElementStructAsserts(SchemaModel schema, ClassModel type, Action<string> Line)
    {
        foreach (FieldModel field in type.Fields.Where(field => field.IsArray && field.StructTarget))
        {
            string element = $"GeneratedType<::{schema.Find(field.Reference!).NativeName}>";
            string symbol = $"{field.Owner}::{field.Name}";
            if (field.ContainerOf(schema.Find) == ArrayRules.StructRefArray)
            {
                Line($"static_assert({element}::ContainsStrongReference && !{element}::ContainsReferenceArray, \"StructRefArray field {symbol} needs a reference struct element without reference arrays; regenerate metadata (RMC2304)\");");
            }
            else
            {
                Line($"static_assert(!{element}::ContainsStrongReference, \"Array field {symbol} holds reference struct {field.Reference}; use StructRefArray and regenerate metadata (RMC2304)\");");
            }
        }
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
    private static void EmitNativeStructAsserts(SchemaModel schema, ClassModel type, NativeMemberModel member, bool worldOnlyCheck, Action<string> Line)
    {
        if (member.Struct is null)
        {
            return;
        }

        string structure = schema.Find(member.Struct).NativeName;
        Line($"static_assert(!GeneratedType<::{structure}>::ContainsStrongReference, \"ROCKS_NATIVE member {type.Name}::{member.Name} holds reference struct {member.Struct} (RMC2205)\");");
        if (worldOnlyCheck)
        {
            Line($"static_assert(!GeneratedType<::{structure}>::RequiresWorldOnlyOwner, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner (RMC2206)\");");
        }
    }

    // 필드의 C++ 형식 철자를 생성 코드에서 쓸 수 있게 다듬는다(한정 이름은 전역 한정 ::을 붙인다).
    private static string CppType(string spelling) =>
        spelling.Contains("::", StringComparison.Ordinal) && !spelling.StartsWith("::", StringComparison.Ordinal) && !spelling.Contains(' ', StringComparison.Ordinal)
            ? "::" + spelling : spelling;

    // Binds 미러와 실제 타입의 대조다(Docs/Design/ReflectionFoundations.ko.md 4.6절 Binds). 비템플릿 함수 안이라 항상
    // 검사된다: 멤버마다 실제 타입의 같은 이름 멤버가 같은 형식이고 실제 타입에 직접 선언되었는지(base에 있으면 멤버
    // 포인터가 T Base::*라 멤버 thunk가 base를 소유자로 본다), 구조적 바인딩으로 공개 데이터 멤버의 개수(private 데이터·
    // union·base 분할도 컴파일 실패)와 위치별 형식, standard layout과 offset 증가 사슬로 선언 순서(같은 형식 멤버끼리
    // 바뀐 순서 포함)를 확인한다. 생성·소멸 noexcept는 MakeRuntimeStructOps가 실제 타입에 건다. sizeof는 대조하지
    // 않는다(꼬리 패딩에 숨은 멤버를 놓친다 — 숨은 멤버는 구조적 바인딩 개수가 잡는다).
    private static void EmitBindsChecks(ClassModel type, string native, Action<string> Line)
    {
        string names = string.Join(", ", type.OwnFields.Select((_, index) => $"m{index}"));
        Line($"static void VerifyBinds(const {native}& value) noexcept {{");
        Line($"using BoundType = {native};");
        Line($"static_assert(__is_standard_layout(BoundType), \"Binds target {type.Binds} must be a standard-layout type\");");
        Line($"[[maybe_unused]] const auto& [{names}] = value;");
        for (int index = 0; index < type.OwnFields.Count; ++index)
        {
            FieldModel field = type.OwnFields[index];
            string member = CppType(field.NativeSpelling);
            Line($"static_assert(__is_same(decltype(BoundType::{field.Name}), {member}), \"Binds mirror {type.Name}::{field.Name} must have the type of {type.Binds}::{field.Name}\");");
            Line($"static_assert(__is_same(decltype(&BoundType::{field.Name}), {member} (BoundType::*)), \"Binds mirror {type.Name}::{field.Name} must be declared directly in {type.Binds}, not in a base\");");
            Line($"static_assert(__is_same(__remove_cv(decltype(m{index})), {member}), \"Binds mirror {type.Name}: member {index} of {type.Binds} must be {field.Name} with the mirror's type (same count and order)\");");
            if (index != 0)
            {
                Line($"static_assert(ROCKS_OFFSET_OF(BoundType, {type.OwnFields[index - 1].Name}) < ROCKS_OFFSET_OF(BoundType, {field.Name}), \"Binds mirror {type.Name}: {type.OwnFields[index - 1].Name} must come before {field.Name} as in {type.Binds}\");");
            }
        }

        Line("}");
    }

    // 리플렉션 struct의 GeneratedType 특수화다(Docs/Design/ReflectionFoundations.ko.md 4.6절): 형질
    // (ContainsStrongReference·RequiresWorldOnlyOwner)과 중첩 깊이, Id, Schema(base·클래스 플래그 없는 TypeSchema),
    // Trace(모든 struct — 참조 없는 struct는 빈 본문), Ops(RuntimeStructOps — 멤버 codec 함수표, 참조 struct만 trace,
    // Migrate가 있으면 migrate), Register. 형질과 깊이는 모델 값을 내되 직접 멤버의 생성 값을 포함하는지
    // static_assert로 대조한다(import descriptor의 flags를 믿지 않는다). Binds 미러는 실제 타입(NativeName)의
    // 특수화를 내고 멤버 thunk·Trace·생성·소멸이 모두 실제 타입을 쓰며, 스키마 이름만 미러 이름이다.
    private static void EmitStruct(SchemaModel schema, ClassModel type, Action<string> Line)
    {
        string native = "::" + type.NativeName;
        string True(bool value) => value ? "true" : "false";
        Line($"template <> struct GeneratedType<{native}> {{");
        Line($"static constexpr bool ContainsStrongReference = {True(type.ContainsStrongReference)};");
        Line($"static constexpr bool RequiresWorldOnlyOwner = {True(type.RequiresWorldOnlyOwner)};");
        Line($"static constexpr bool ContainsReferenceArray = {True(type.ContainsReferenceArray)};");
        Line($"static constexpr uint32 Depth = {type.Depth}u;");
        Line($"static_assert(Depth <= ::rocks::MaxStructDepth, \"ROCKS_STRUCT {type.Name} nesting depth exceeds MaxStructDepth (RMC2303)\");");
        if (type.Binds.Length != 0)
        {
            EmitBindsChecks(type, native, Line);
        }

        // 멤버 struct와 원소 struct는 깊이(원소 배열 멤버는 원소 깊이 + 1)와 형질을 포함해야 한다.
        foreach (string structure in type.Fields.Where(field => field.StructTarget).Select(field => field.Reference!).Distinct(StringComparer.Ordinal))
        {
            string member = $"GeneratedType<::{schema.Find(structure).NativeName}>";
            Line($"static_assert({member}::Depth < Depth, \"ROCKS_STRUCT {type.Name} nests {structure} deeper than recorded; regenerate metadata (RMC2303)\");");
            Line($"static_assert(ContainsStrongReference || !{member}::ContainsStrongReference, \"ROCKS_STRUCT {type.Name} holds reference struct {structure}; regenerate metadata\");");
            Line($"static_assert(RequiresWorldOnlyOwner || !{member}::RequiresWorldOnlyOwner, \"ROCKS_STRUCT {type.Name} holds struct {structure} that requires a WorldOnly owner; regenerate metadata\");");
        }

        foreach (string structure in type.Fields.Where(field => field.IsStruct).Select(field => field.Reference!).Distinct(StringComparer.Ordinal))
        {
            Line($"static_assert(ContainsReferenceArray || !GeneratedType<::{schema.Find(structure).NativeName}>::ContainsReferenceArray, \"ROCKS_STRUCT {type.Name} holds struct {structure} with reference arrays; regenerate metadata\");");
        }

        EmitElementStructAsserts(schema, type, Line);

        // 모델 형질을 C++ 멤버 타입과 직접 대조한다(encode 고정이 ContainsReferenceArray에 기대므로 틀린 모델 값이 고정
        // 누락이 되지 않게 — Docs/Design/ReflectionFoundations.ko.md 5.6절). 직접 타입만 본다(struct 멤버는 위의 생성 형질).
        // Binds 미러는 생략한다: VerifyBinds가 실제 타입의 멤버마다 __is_same(decltype(Real::m), 미러 필드 형식)을 확인하고 모델
        // 형질은 그 미러 필드 형식으로만 계산되므로 같은 보장이 이미 있다.
        foreach (FieldModel field in type.Binds.Length == 0 ? type.Fields : [])
        {
            string memberType = $"decltype({native}::{field.Name})";
            Line($"static_assert(ContainsStrongReference || !::rocks::IsNativeStrongReference<{memberType}>, \"ROCKS_STRUCT {type.Name}::{field.Name} holds a strong object reference; regenerate metadata\");");
            Line($"static_assert(ContainsReferenceArray || !::rocks::detail::RegistryContainer<{memberType}>::value, \"ROCKS_STRUCT {type.Name}::{field.Name} is a reference array; regenerate metadata\");");
        }

        // NATIVE 멤버는 스키마에 넣지 않는다. 강한 참조 금지(RMC2205)를 직접 타입에 대해 한 번 더 확인하고, WorldOnly
        // 소유자 형질은 struct에서는 오류가 아니라 RequiresWorldOnlyOwner로 전파되었는지 확인한다.
        foreach (NativeMemberModel member in type.NativeMembers)
        {
            string memberType = $"decltype({native}::{member.Name})";
            Line($"static_assert(!IsNativeStrongReference<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} must not hold a strong object reference (RMC2205)\");");
            Line($"static_assert(RequiresWorldOnlyOwner || !::rocks::RequiresWorldOnlyOwner<{memberType}>, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner; regenerate metadata\");");
            EmitNativeStructAsserts(schema, type, member, false, Line);
            if (member.Struct is not null)
            {
                Line($"static_assert(RequiresWorldOnlyOwner || !GeneratedType<::{schema.Find(member.Struct).NativeName}>::RequiresWorldOnlyOwner, \"ROCKS_NATIVE member {type.Name}::{member.Name} requires a WorldOnly owner; regenerate metadata\");");
            }
        }

        Line($"static constexpr TypeId Id() noexcept {{ return {GuidValue(type.Id)}; }}");
        Line("static const TypeSchema& Schema() noexcept {");
        EmitFieldSchemas(type, Line);
        string fieldSpan = type.Fields.Count == 0 ? "Span<const FieldSchema>{}" : $"Span<const FieldSchema>(fields, {type.Fields.Count})";
        Line($"static const TypeSchema schema{{Id(), {type.Version}, {fieldSpan}, {View(type.Name)}, Guid{{}}, 0u}};");
        Line("return schema;");
        Line("}");
        // 참조 struct는 직접 ObjectRef 멤버(Transient 포함 — GC는 추적한다)를 Visit하고 멤버 struct의 Trace를, 모든 배열
        // 멤버에 TraceArrayField를 부른다(인라인 struct 안 ObjectRefArray·StructRefArray). 참조 없는 struct는 빈 본문이다(그
        // 형질은 위의 static_assert가 C++ 멤버 타입과 대조한다).
        FieldModel[] traced = type.ContainsStrongReference ? type.Fields.Where(field => field.IsReference || field.IsStruct || field.IsArray).ToArray() : [];
        if (traced.Length == 0)
        {
            Line($"static void Trace(const {native}&, TraceVisitor&) noexcept {{}}");
        }
        else
        {
            Line($"static void Trace(const {native}& value, TraceVisitor& visitor) noexcept {{");
            foreach (FieldModel field in traced)
            {
                Line(TraceStatement(schema, field, "value", native));
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

    // 클래스·struct 스키마의 필드 배열이다(필드가 없으면 내지 않는다). 배열 필드는 value가 {Array, 0}이고 원소 형식
    // element(원소 대상 ID)와 maxElements를 더 싣는다(Docs/Design/ReflectionFoundations.ko.md 5.6절).
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
            string kind = field.IsObjectReferenceFieldKind ? "ObjectReference" : "Value";
            string head = $"{{{GuidValue(field.Id)}, FieldKind::{kind}, {field.Flags}u, {View(field.Name)}, {{}}, ";
            string labels = $"{View(field.Spec.Category)}, {View(field.Spec.DisplayName)}, {View(field.Spec.ToolName)}";
            Line(field.IsArray
                ? $"{head}{{ValueKind::Array, Guid{{}}}}, {labels}, {{ValueKind::{field.ElementKind}, {GuidValue(targetId)}}}, {field.MaxElements}u}},"
                : $"{head}{{ValueKind::{field.Kind}, {GuidValue(targetId)}}}, {labels}}},");
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
