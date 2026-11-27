using System.Globalization;

namespace Rocks.MetaCompiler;

// 리플렉션 함수(ROCKS_FUNCTION — Docs/Design/FunctionReflection.ko.md FD7·FD10)의 생성 코드다. 함수마다 호출 frame struct(매개변수 → 결과 →
// 실패 코드), invoke thunk(GeneratedType<T>의 static 멤버 — friend라 접근 수준과 무관하다), 실행 schema(FunctionSchema·ParameterSchema)와
// 연산표(RuntimeFunctionOps·RuntimeParameterOps)를 낸다. EditorOnly 함수는 #if ROCKS_WITH_EDITOR로 감싸고, 배열은 끝 표지를 두어 모든 항목이
// 빠진 조합에서도 크기가 0이 아니다(span 크기는 표지를 뺀다).
internal static partial class CppEmitter
{
    private const string EditorCondition = "ROCKS_WITH_EDITOR";

    // 호출 frame struct 이름이다. 클래스 정규 이름의 마디와 함수 이름을 길이 접두로 잇는다(`game::Door::Open_Fast` →
    // RocksFrame_4game4Door9Open_Fast) — `_`로 잇기만 하면 `game::Door_Open::Fast`와 겹쳐 다른 생성 소스의 frame과 같은 외부 이름이 된다.
    private static string FrameName(ClassModel type, FunctionModel function) =>
        "RocksFrame_" + string.Concat(type.Name.Split("::").Append(function.Name).Select(part => part.Length.ToString(CultureInfo.InvariantCulture) + part));

    private static string InvokeName(FunctionModel function) => "RocksInvoke_" + function.Name;

    // 칸의 frame 멤버 C++ 형식이다(문자열 view는 String으로 잡는다). 정규 철자는 전역에서 찾도록 한정 이름이면 ::를 붙인다.
    private static string SlotType(FieldModel slot) =>
        slot.NativeSpelling == FunctionRules.StringViewSpelling ? "::" + FunctionRules.StringSpelling
        : slot.NativeSpelling.Contains("::", StringComparison.Ordinal) && !slot.NativeSpelling.StartsWith("::", StringComparison.Ordinal) ? "::" + slot.NativeSpelling
        : slot.NativeSpelling;

    // 함수 칸을 담은 함수 줄을 EditorOnly면 #if ROCKS_WITH_EDITOR로 감싼다.
    private static void FunctionGuarded(FunctionModel function, Action<string> sink, Action emit)
    {
        if (function.Spec.EditorOnly)
        {
            sink($"#if {EditorCondition}");
        }

        emit();
        if (function.Spec.EditorOnly)
        {
            sink("#endif");
        }
    }

    // 헤더의 GeneratedType<T> 안 thunk 선언이다.
    private static void EmitFunctionDeclarations(ClassModel type, Action<string> Line)
    {
        foreach (FunctionModel function in type.Functions)
        {
            FunctionGuarded(function, Line, () => Line(
                $"static Result<void> {InvokeName(function)}(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle self, void* frame) noexcept;"));
        }
    }

    // 생성 소스의 frame struct·thunk 정의와 형식 static_assert다(Schema 앞).
    private static void EmitFunctionDefinitions(SchemaModel schema, ClassModel type, string native, string self, Action<string> Body)
    {
        foreach (FunctionModel function in type.Functions)
        {
            FunctionGuarded(function, Body, () =>
            {
                string frame = FrameName(type, function);
                List<(string Name, FieldModel Slot)> members = function.Parameters.Select((slot, index) => ($"p{index}", slot)).ToList();
                if (function.Result is FieldModel result)
                {
                    members.Add(("result", result));
                }

                // 강한 참조를 담는 struct는 호출 frame(Registry 밖 버퍼)에 둘 수 없다(RMC2704 — struct 형질은 생성 코드가 안다).
                foreach ((string _, FieldModel slot) in members.Where(member => member.Slot.StructTarget && member.Slot.Reference is not null))
                {
                    string structure = "::" + schema.Find(slot.Reference!).NativeName;
                    Body($"static_assert(!GeneratedType<{structure}>::ContainsStrongReference, \"{function.Owner}::{function.Name}: a struct holding object references cannot be a reflected function parameter or result (RMC2704)\");");
                }

                // 값 매개변수는 frame의 인자를 복사해 넘긴다(Invoke는 같은 frame으로 여러 번 부른다 — 인자가 남는다).
                foreach (int index in function.ByValueParameters.Order())
                {
                    Body($"static_assert(IsCopyConstructibleV<{SlotType(function.Parameters[index])}>, \"{function.Owner}::{function.Name}: parameter " +
                        $"'{function.Parameters[index].Name}' cannot be copied; take it as const& (RMC2704)\");");
                }

                // 칸은 detail::FrameSlot(정렬된 byte 저장소)이라 값 형식이 표준 배치가 아니어도 frame은 표준 배치다(offsetof — 실행 schema의 칸
                // 위치). 생성자가 선언 순서로 값을 만들고 소멸자가 거꾸로 부순다.
                Body("namespace detail::generated_frames {");
                Body($"struct {frame} {{");
                if (members.Count == 0)
                {
                    Body($"explicit {frame}(Allocator&) noexcept {{}}");
                }
                else
                {
                    Body($"explicit {frame}(Allocator& allocator) noexcept {{");
                    foreach ((string name, FieldModel _) in members)
                    {
                        Body($"{name}.Construct(allocator);");
                    }

                    Body("}");
                    Body($"~{frame}() noexcept {{");
                    foreach ((string name, FieldModel _) in Enumerable.Reverse(members))
                    {
                        Body($"{name}.Destroy();");
                    }

                    Body("}");
                }

                Body($"{frame}(const {frame}&) = delete;");
                Body($"{frame}& operator=(const {frame}&) = delete;");
                foreach ((string name, FieldModel slot) in members)
                {
                    Body($"detail::FrameSlot<{SlotType(slot)}> {name};");
                }

                Body("ErrorCode error = ErrorCode::None;");
                Body("};");
                Body("} // namespace detail::generated_frames");
                Body($"Result<void> {self}::{InvokeName(function)}(ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle self, void* frame) noexcept {{");
                // 인자·결과·실패 코드가 없는 함수는 frame을 쓰지 않는다.
                Body(members.Count != 0 || function.ReturnsResult
                    ? $"auto& f = *static_cast<detail::generated_frames::{frame}*>(frame);"
                    : "static_cast<void>(frame);");
                string arguments = string.Join(", ", function.Parameters.Select((slot, index) =>
                    slot.NativeSpelling == FunctionRules.StringViewSpelling ? $"f.p{index}.Get().View()" : $"f.p{index}.Get()"));
                string call;
                if (function.IsStatic)
                {
                    Body("static_cast<void>(self);");
                    Body("if (auto owned = registry.VerifyAccess(access); !owned) {");
                    Body("return owned;");
                    Body("}");
                    call = $"{native}::{function.Name}({arguments})";
                }
                else
                {
                    // const 함수는 대상을 읽기로, 그 밖은 쓰기로 해석한다(FD8 — encode 고정 중이면 Busy, MutationStamp가 오른다).
                    Body($"auto borrow = registry.{(function.IsConst ? "Resolve" : "ResolveForWrite")}<{native}>(access, self);");
                    Body("if (!borrow) {");
                    Body("return Result<void>::Failure(borrow.Error());");
                    Body("}");
                    Body("auto target = borrow.Value().Get();");
                    Body("if (!target) {");
                    Body("return Result<void>::Failure(target.Error());");
                    Body("}");
                    call = $"target.Value()->{function.Name}({arguments})";
                }

                if (function.ReturnsResult)
                {
                    Body($"auto done = {call};");
                    Body("if (!done) {");
                    Body("f.error = done.Error();");
                    Body("return Result<void>::Success();");
                    Body("}");
                    if (function.Result is not null)
                    {
                        Body("f.result.Get() = Move(done.Value());");
                    }
                }
                else if (function.Result is not null)
                {
                    Body($"f.result.Get() = {call};");
                }
                else
                {
                    Body($"{call};");
                }

                Body("return Result<void>::Success();");
                Body("}");
            });
        }
    }

    // 칸 하나의 ParameterSchema 초기화다({이름, 값, 원소, 키, flags, maxElements, offset}).
    private static string SlotSchema(FieldModel slot, string frame, string member) =>
        SlotSchemaAt(slot, $"static_cast<uint32>(offsetof(detail::generated_frames::{frame}, {member}))");

    // 칸 하나의 ParameterSchema 초기화이고 offset 식을 받는다(함수 frame은 offsetof, 이벤트 frame은 EventFrameShape — EventReflection ED17).
    private static string SlotSchemaAt(FieldModel slot, string offset)
    {
        string value = slot.HasElement
            ? $"{{ValueKind::{slot.Kind}, Guid{{}}}}"
            : $"{{ValueKind::{slot.Kind}, {(slot.HasTargetType ? GuidValue(slot.ReferenceId) : "Guid{}")}}}";
        string element = slot.HasElement ? $"{{ValueKind::{slot.ElementKind}, {(slot.HasTargetType ? GuidValue(slot.ReferenceId) : "Guid{}")}}}" : "{}";
        string key = slot.IsMap ? $"{{ValueKind::{slot.KeyKind}, {(slot.KeyHasTargetType ? GuidValue(slot.KeyReferenceId) : "Guid{}")}}}" : "{}";
        // ObjectHandle 칸은 ParameterFlags::ObjectHandle(값 형식이 WeakObjectRef<Object>와 같아 바인딩 서명 비교가 이 flag로 가른다 — EventReflection ED5).
        string flags = slot.NativeSpelling == FunctionRules.ObjectHandleSpelling ? "static_cast<uint32>(ParameterFlags::ObjectHandle)" : "0u";
        return $"{{{View(slot.Name)}, {value}, {element}, {key}, {flags}, {(slot.IsCounted ? slot.MaxElements : 0)}u, {offset}}}";
    }

    // Schema() 안의 함수 서술 배열이다. 함수가 있으면 TypeSchema 초기화의 functions 칸 문자열을, 없으면 ""를 돌려준다.
    private static string EmitFunctionSchemas(ClassModel type, Action<string> Body)
    {
        if (type.Functions.Count == 0)
        {
            return "";
        }

        foreach (FunctionModel function in type.Functions.Where(function => function.Parameters.Count != 0))
        {
            string frame = FrameName(type, function);
            FunctionGuarded(function, Body, () => Body(
                $"static const ParameterSchema rocksParameters_{function.Name}[] = {{{string.Join(", ", function.Parameters.Select((slot, index) => SlotSchema(slot, frame, $"p{index}")))}}};"));
        }

        Body("static const FunctionSchema functions[] = {");
        foreach (FunctionModel function in type.Functions)
        {
            string frame = FrameName(type, function);
            string parameters = function.Parameters.Count == 0 ? "{}" : $"Span<const ParameterSchema>(rocksParameters_{function.Name}, {function.Parameters.Count})";
            string result = function.Result is FieldModel slot
                ? SlotSchema(slot, frame, "result").Replace(View(slot.Name), "{}", StringComparison.Ordinal)
                : "{}";
            string toolName = function.Spec.ToolName.Length == 0 ? "{}" : View(function.Spec.ToolName);
            FunctionGuarded(function, Body, () => Body(
                $"FunctionSchema{{{GuidValue(function.Id)}, {View(function.Name)}, {toolName}, {function.Flags}u, {parameters}, {result}, " +
                $"static_cast<uint32>(sizeof(detail::generated_frames::{frame})), static_cast<uint32>(alignof(detail::generated_frames::{frame}))}},"));
        }

        // 끝 표지(모든 함수가 EditorOnly인 게임 조합에서도 배열 크기가 0이 아니다 — span이 뺀다).
        Body("FunctionSchema{},");
        Body("};");
        return "Span<const FunctionSchema>(functions, sizeof(functions) / sizeof(functions[0]) - 1)";
    }

    // Ops() 안의 함수 연산표다.
    private static void EmitFunctionOps(ClassModel type, string self, Action<string> Body)
    {
        if (type.Functions.Count == 0)
        {
            return;
        }

        foreach (FunctionModel function in type.Functions.Where(function => function.Parameters.Count != 0))
        {
            FunctionGuarded(function, Body, () => Body(
                $"static const RuntimeParameterOps rocksParameterOps_{function.Name}[] = {{{string.Join(", ", function.Parameters.Select(slot => $"detail::ParameterOpsOf<{SlotType(slot)}>()"))}}};"));
        }

        Body("static const RuntimeFunctionOps functions[] = {");
        foreach (FunctionModel function in type.Functions)
        {
            string frame = $"detail::generated_frames::{FrameName(type, function)}";
            string parameters = function.Parameters.Count == 0 ? "{}" : $"Span<const RuntimeParameterOps>(rocksParameterOps_{function.Name}, {function.Parameters.Count})";
            string result = function.Result is FieldModel slot ? $"detail::ParameterOpsOf<{SlotType(slot)}>()" : "{}";
            string error = function.ReturnsResult ? $"static_cast<uint32>(offsetof({frame}, error))" : "0u";
            FunctionGuarded(function, Body, () => Body(
                $"RuntimeFunctionOps{{{GuidValue(function.Id)}, &detail::ConstructFunctionFrame<{frame}>, &detail::DestroyFunctionFrame<{frame}>, " +
                $"&{self}::{InvokeName(function)}, nullptr, {parameters}, {result}, {error}}},"));
        }

        Body("RuntimeFunctionOps{},");
        Body("};");
        Body("ops.functions = Span<const RuntimeFunctionOps>(functions, sizeof(functions) / sizeof(functions[0]) - 1);");
    }
}
