namespace Rocks.MetaCompiler;

internal sealed record SourceSite(string File, int Line);

internal sealed class MetadataException(string message, string code = "RMC2000", string? hint = null) : Exception(message)
{
    public string Code { get; } = code;

    public string Hint { get; } = hint ?? "Correct the named declaration using the supported CLASS/PROPERTY syntax; unsupported constructs must be replaced or explicitly implemented.";

    public SourceSite? Site { get; private set; }

    public MetadataException At(SourceSite site)
    {
        Site ??= site;
        return this;
    }
}

internal sealed class FieldModel
{
    public required string Id { get; init; }

    public required SourceSite Site { get; init; }

    public required string Name { get; init; }

    public required string Owner { get; init; }

    public required string NativeSpelling { get; init; }

    public required PropertySpec Spec { get; init; }

    // 값 종류(ValueKind 이름)다. 배열 필드는 "Array"이고 원소 종류는 ElementKind다.
    public string Kind { get; set; } = "Opaque";

    // 배열 필드(Kind "Array")의 원소 값 종류다(배열이 아니면 null). 배열의 value.type은 항상 0이므로 원소의 대상
    // (enum·struct·참조 대상)은 Reference·ReferenceId에 둔다(Docs/Design/ReflectionFoundations.ko.md 5.6절 —
    // 대상을 찾는 지점이 배열에서 null 역참조나 조용한 건너뜀이 되지 않게 별도 칸을 두지 않는다).
    public string? ElementKind { get; set; }

    // 배열 필드의 길이 상한이다(MaxElements 지정자, 없으면 ArrayRules.DefaultMaxElements). 배열이 아니면 0이다.
    // import 필드는 descriptor 값을 그대로 보존한다(파생 클래스 스키마가 상속 maxElements를 같은 값으로 내야 한다).
    public uint MaxElements { get; set; }

    // 이 모듈 배열 필드의 C++ 철자가 쓴 컨테이너(ArrayRules.Array·ObjectRefArray·StructRefArray)다. 파생 속성
    // ContainerOf와 다르면 RMC2304다. import 필드와 배열이 아닌 필드는 null이다(import 필드의 컨테이너는 원소
    // 종류와 원소 struct 형질로 복원한다).
    public string? SpelledContainer { get; set; }

    // 이 모듈 배열 필드의 원소 C++ 철자다(진단용, 그 밖은 "").
    public string ElementSpelling { get; set; } = "";

    // 이 모듈 안에서 해석한 참조 대상의 정규 이름이다(rocks::Object 포함, 배열이면 원소의 대상). import 필드는
    // null이고, import 필드의 Enum·Struct 대상(원소 포함)만 import를 읽을 때 대상 ID로 채운다(상속한 enum·struct
    // 필드와 import struct의 멤버를 생성·검사할 때 쓴다 — SchemaValidator.LinkImported).
    public string? Reference { get; set; }

    // 참조 대상 타입 ID(32자리 16진, Object는 null, 배열이면 원소의 대상)다. import 필드는 descriptor에서 바로 온다.
    public string? ReferenceId { get; set; }

    // 단일 ObjectRef 필드다(배열 원소는 IsObjectReferenceFieldKind).
    public bool IsReference => Kind == "ObjectReference";

    // 리플렉션 struct 값 필드다(중첩 Record — struct 원소 배열은 StructTarget).
    public bool IsStruct => Kind == "Struct";

    // 배열 필드(Array·ObjectRefArray·StructRefArray)다.
    public bool IsArray => Kind == "Array";

    // 필드 값의 대상 종류다: 배열이면 원소 종류, 아니면 값 종류(런타임 FieldTargetType과 같은 규칙).
    public string TargetKind => IsArray ? ElementKind ?? "Opaque" : Kind;

    // struct 필드이거나 struct 원소 배열이다(깊이·순위·형질·생성 헤더 include의 간선).
    public bool StructTarget => TargetKind == "Struct";

    // FieldKind::ObjectReference인 필드다: ObjectRef 필드이거나 ObjectRefArray(원소 ObjectReference). StructRefArray는
    // 아니다(값 필드이고 원소 struct가 참조를 담는다).
    public bool IsObjectReferenceFieldKind => TargetKind == "ObjectReference";

    // 대상 타입 ID를 쓰는 종류다(배열이면 원소): 강한 참조·소프트 참조의 기대 타입, Enum의 enum ID, Struct의
    // struct ID. 스키마·descriptor에는 배열이면 element.type, 아니면 value.type으로 나간다.
    public bool HasTargetType => KindHasTargetType(TargetKind);

    // Required를 쓸 수 있는 종류다: 강한 참조와 소프트 참조, 그리고 그 원소의 배열(원소마다 non-null).
    public bool AcceptsRequired => KindAcceptsRequired(TargetKind);

    // 배열 필드의 컨테이너다(저장하지 않는 파생 속성 — Docs/Design/ReflectionFoundations.ko.md 5.6절): 원소가
    // ObjectReference면 ObjectRefArray, 원소가 Struct이고 그 struct가 ContainsStrongReference면 StructRefArray, 그
    // 밖은 Array. 배열이 아니면 null이다. find는 원소 struct의 정규 이름으로 모델을 찾는다.
    public string? ContainerOf(Func<string, ClassModel> find)
    {
        if (!IsArray)
        {
            return null;
        }

        if (ElementKind == "ObjectReference")
        {
            return ArrayRules.ObjectRefArray;
        }

        return ElementKind == "Struct" && find(Reference!).ContainsStrongReference ? ArrayRules.StructRefArray : ArrayRules.Array;
    }

    // Registry가 관리하는 컨테이너(ObjectRefArray·StructRefArray) 필드다.
    public bool IsRegistryContainer(Func<string, ClassModel> find) => ContainerOf(find) is ArrayRules.ObjectRefArray or ArrayRules.StructRefArray;

    // descriptor 읽기·쓰기와 생성이 같은 조건을 쓰도록 종류 이름으로도 판정한다.
    public static bool KindHasTargetType(string kind) => kind is "ObjectReference" or "SoftObjectReference" or "Enum" or "Struct";

    // 대상 ID가 비어 있으면 안 되는 종류다(참조의 빈 ID는 Object다).
    public static bool KindRequiresTargetId(string kind) => kind is "Enum" or "Struct";

    public static bool KindAcceptsRequired(string kind) => kind is "ObjectReference" or "SoftObjectReference";

    // FieldFlags 비트: Transient 1, ReadOnly 2, EditorOnly 4, Required 8.
    public uint Flags => (Spec.Transient ? 1u : 0u) | (Spec.ReadOnly ? 2u : 0u) | (Spec.EditorOnly ? 4u : 0u) | (Spec.Required ? 8u : 0u);

    // 도구 이름(지정하지 않았으면 필드 이름)이다. 상속 포함 대소문자 무시로 유일해야 한다(RMC2203).
    public string EffectiveToolName => Spec.ToolName.Length != 0 ? Spec.ToolName : Name;
}

// 리플렉션 배열 필드 규칙을 한 곳에 둔다(Docs/Design/ReflectionFoundations.ko.md 5.5·5.6절). 길이 상한은 런타임
// MetaSchema의 DefaultMaxElements·MaxArrayElements와 같은 값이다.
internal static class ArrayRules
{
    public const uint DefaultMaxElements = 65536;

    public const uint MaxArrayElements = 1048576;

    public const string Array = "Array";

    public const string ObjectRefArray = "ObjectRefArray";

    public const string StructRefArray = "StructRefArray";

    // C++ 컨테이너 철자의 템플릿 이름(desugar 철자, 앞의 :: 없음)과 컨테이너 이름이다.
    private static readonly (string Template, string Container)[] Containers =
    [
        ("rocks::Array", Array),
        ("rocks::ObjectRefArray", ObjectRefArray),
        ("rocks::StructRefArray", StructRefArray)
    ];

    // spelling이 rocks::Array<…>·rocks::ObjectRefArray<…>·rocks::StructRefArray<…>면 (컨테이너, 원소 철자)를 돌려준다
    // (아니면 null). 템플릿 인자는 꺾쇠 깊이로 나누며 인자가 정확히 하나여야 한다(ObjectRefArray<>의 원소 철자는 "").
    public static (string Container, string Element)? Split(string spelling)
    {
        string text = spelling.Trim();
        text = text.StartsWith("::", StringComparison.Ordinal) ? text[2..] : text;
        foreach ((string template, string container) in Containers)
        {
            if (!text.StartsWith(template + "<", StringComparison.Ordinal))
            {
                continue;
            }

            int open = template.Length;
            int depth = 0;
            for (int index = open; index < text.Length; ++index)
            {
                char c = text[index];
                if (c is '<' or '(' or '[')
                {
                    ++depth;
                }
                else if (c is '>' or ')' or ']')
                {
                    --depth;
                    if (depth == 0)
                    {
                        // 닫는 꺾쇠는 철자 끝이어야 하고(rocks::Array<int>::Inner 같은 중첩 이름이 아니게) 인자는 하나다.
                        string argument = text[(open + 1)..index].Trim();
                        return index == text.Length - 1 && !HasTopLevelComma(argument) ? (container, argument) : null;
                    }
                }
            }

            return null;
        }

        return null;
    }

    // spelling이 리플렉션 배열 컨테이너 철자인지다(중첩 배열·컨테이너 인자의 배열 판정 — RMC2304).
    public static bool IsContainer(string spelling) => Split(spelling) is not null;

    // 원소 철자가 맨 위에서 cv 한정인지다(Array<const T>·Array<T* const>는 RMC2000). 포인터·참조 대상의 const
    // (Array<const T*>)는 원소 자체의 cv가 아니므로 거짓이다(지원하지 않는 원소 형식으로 진단한다).
    public static bool IsCvQualified(string spelling)
    {
        string text = spelling.Trim();
        if (text.EndsWith(" const", StringComparison.Ordinal) || text.EndsWith(" volatile", StringComparison.Ordinal))
        {
            return true;
        }

        bool qualifiedHead = text.StartsWith("const ", StringComparison.Ordinal) || text.StartsWith("volatile ", StringComparison.Ordinal);
        return qualifiedHead && !text.EndsWith('*') && !text.EndsWith('&');
    }

    // RMC2304 진단의 공통 안내다(Core Array.h의 메시지는 ObjectRuntime 타입 이름을 모르게 중립으로 둔다).
    public const string ElementHint =
        "Hold object references in rocks::ObjectRefArray<T> and reference structs in rocks::StructRefArray<S>; arrays do not nest (wrap an inner array in a ROCKS_STRUCT element instead).";

    private static bool HasTopLevelComma(string text)
    {
        int depth = 0;
        foreach (char c in text)
        {
            if (c is '<' or '(' or '[')
            {
                ++depth;
            }
            else if (c is '>' or ')' or ']')
            {
                --depth;
            }
            else if (c == ',' && depth == 0)
            {
                return true;
            }
        }

        return false;
    }
}

// enum 규칙을 한 곳에 둔다. 파서·descriptor·생성기가 같이 쓰며 런타임 TypeRegistry::RegisterEnum과 같은 규칙이다.
internal static class EnumRules
{
    public const int MaxEnumerators = 4096;

    // underlying(ValueKind 이름)의 값 범위다. 지원하지 않는 underlying이면 null이다.
    public static (long Minimum, long Maximum)? Range(string underlying) => underlying switch
    {
        "UInt8" => (0L, 0xffL),
        "UInt16" => (0L, 0xffffL),
        "UInt32" => (0L, 0xffffffffL),
        "Int32" => (int.MinValue, int.MaxValue),
        _ => null
    };

    // underlying의 rocks 정수 타입 이름이다(생성 static_assert용).
    public static string NativeType(string underlying) => underlying switch
    {
        "UInt8" => "uint8",
        "UInt16" => "uint16",
        "UInt32" => "uint32",
        _ => "int32"
    };

    // 접두 유무를 합친 두 도구 이름 집합 {a, prefix+a}와 {b, prefix+b}가 ASCII 대소문자 무시로 겹치는지다.
    public static bool ToolNamesCollide(string left, string right, string prefix) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase) ||
        (prefix.Length != 0 && (string.Equals(prefix + left, right, StringComparison.OrdinalIgnoreCase) ||
                                string.Equals(left, prefix + right, StringComparison.OrdinalIgnoreCase)));
}

// enum 열거자 하나다. ToolName은 Name 지정자(C++ 이름과 같거나 없으면 "")다.
internal sealed record EnumeratorModel(string Name, string ToolName, string DisplayName, long Value, bool Hidden)
{
    public string EffectiveToolName => ToolName.Length != 0 ? ToolName : Name;
}

// ROCKS_NATIVE 멤버다. Spelling은 Clang이 desugar한 타입 철자이고, 직접 타입이 리플렉션 struct면 Struct가 그
// 정규 이름이다(SchemaValidator가 채운다 — 생성 코드가 그 struct의 생성 형질로 RMC2205·2206을 한 번 더 확인한다).
internal sealed class NativeMemberModel
{
    public required string Name { get; init; }

    public required string Spelling { get; init; }

    public required SourceSite Site { get; init; }

    public string? Struct { get; set; }
}

// 리플렉션 struct 규칙을 한 곳에 둔다(Docs/Design/ReflectionFoundations.ko.md 4.6절). 런타임 MaxStructDepth와
// 같은 값이다.
internal static class StructRules
{
    public const int MaxDepth = 16;

    // Binds(Core 값 타입 결합)를 둘 수 있는 유일한 모듈이다(Docs/Design/ReflectionFoundations.ko.md 4.5·4.6절 — 무관한
    // 두 모듈이 같은 타입을 묶으면 ODR 위반·런타임 AlreadyExists가 된다).
    public const string BindingModule = "CoreTypeBindings";

    // CoreTypeBindings가 묶는 Core 값 타입(템플릿은 이름)이다. 그 모듈의 descriptor를 import하지 않은 모듈이 이런 필드를
    // 쓰면 진단에 의존을 더하라는 안내를 붙인다. descriptor가 없을 때는 무엇이 묶였는지 알 수 없어 이 목록을 두며,
    // 안내 문구에만 쓰이므로(해석에는 쓰지 않는다) CoreValueTypes.h와 어긋나도 안내가 약해질 뿐이다. descriptor를
    // import한 경우는 그 binds가 기준이다(SchemaValidator의 안내가 묶이지 않은 인스턴스를 알려 준다).
    private static readonly string[] CoreValueTypes =
    [
        "rocks::Vector2<", "rocks::Vector3<", "rocks::Vector4<", "rocks::Quaternion<", "rocks::Rotator<",
        "rocks::Color", "rocks::LinearColor", "rocks::IntVec2", "rocks::IntVec3", "rocks::Transform"
    ];

    public static bool IsCoreValueType(string spelling)
    {
        string text = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
        return CoreValueTypes.Any(name => name.EndsWith('<') ? text.StartsWith(name, StringComparison.Ordinal) : text == name);
    }

    // Binds 철자(지정자 원문·descriptor binds)의 문법이다: 한정 이름과 템플릿 인자(이름·정수)만, 꺾쇠 짝이 맞고 512자
    // 이하. 합성 번역 단위에 그대로 넣으므로 문장·주석·전처리 문자를 받지 않는다.
    public static bool IsBindsSpelling(string text)
    {
        if (text.Length == 0 || text.Length > 512 || text.Any(c => !(char.IsAsciiLetterOrDigit(c) || c is '_' or ':' or '<' or '>' or ',' or ' ')))
        {
            return false;
        }

        int depth = 0;
        foreach (char c in text)
        {
            depth += c == '<' ? 1 : c == '>' ? -1 : 0;
            // 공백은 템플릿 인자 안(unsigned int 등)에만 둔다.
            if (depth < 0 || (depth == 0 && c is ' ' or ','))
            {
                return false;
            }
        }

        string head = text.StartsWith("::", StringComparison.Ordinal) ? text[2..] : text;
        return depth == 0 && head.Length != 0 && (char.IsAsciiLetter(head[0]) || head[0] == '_') && !text.Contains(":::", StringComparison.Ordinal) &&
            !text.EndsWith(':') && text.Replace("::", "", StringComparison.Ordinal).IndexOf(':') < 0;
    }
}

// 리플렉션 타입(ROCKS_CLASS 클래스, ROCKS_STRUCT struct, ROCKS_ENUM enum) 모델이다. enum은 IsEnum이고 Base는 "",
// 필드는 없으며 Underlying·FlagsEnum·ToolPrefix·Enumerators를 쓴다. struct는 IsStruct이고 Base는 ""이며
// 형질(ContainsStrongReference·RequiresWorldOnlyOwner)과 중첩 깊이(Depth)를 갖는다. 셋 모두 같은 이름·ID 공간과
// descriptor·inventory 경로를 쓴다.
internal sealed class ClassModel
{
    public required string Name { get; init; }

    public required string Id { get; init; }

    public required SourceSite Site { get; init; }

    public required uint Version { get; init; }

    public required string Base { get; set; }

    public required bool Abstract { get; init; }

    // TypeFlags::WorldOnly. 선언이 직접 쓰거나 base에서 상속된다(SchemaValidator가 base를 보고 채운다).
    public bool WorldOnly { get; set; }

    // TypeFlags 비트: Abstract 1, WorldOnly 2.
    public uint Flags => (Abstract ? 1u : 0u) | (WorldOnly ? 2u : 0u);

    // ROCKS_NATIVE 멤버다(스키마에 넣지 않고 생성 코드의 static_assert만 받는다).
    public List<NativeMemberModel> NativeMembers { get; } = [];

    // RocksRequiresWorldOnlyOwner 형질을 담는 NATIVE 멤버다. 클래스는 상속까지 본 WorldOnly가 아니면 RMC2206이고,
    // struct는 오류가 아니라 RequiresWorldOnlyOwner 형질이 된다.
    public List<(string Symbol, string Path, SourceSite Site)> WorldOnlyNatives { get; } = [];

    public bool IsStruct { get; init; }

    // struct 형질: 강한 참조(ObjectReference 필드 — Transient 포함 — 또는 그런 struct 멤버)를 담는다. 이 모듈
    // struct는 SchemaValidator가 계산하고 import struct는 descriptor flags에서 온다(LinkImported가 필드와 대조한다).
    public bool ContainsStrongReference { get; set; }

    // struct 형질: WorldOnly 소유자를 요구하는 NATIVE 멤버나 그런 struct 멤버(원소 struct 포함)를 담는다. import
    // struct는 descriptor flags에서 온다(NATIVE 멤버는 descriptor에 없으므로 멤버 struct의 형질을 포함하는지만
    // 대조한다).
    public bool RequiresWorldOnlyOwner { get; set; }

    // struct 형질: Registry 컨테이너(ObjectRefArray·StructRefArray) 멤버나 그런 struct 멤버를 (중첩까지) 담는다.
    // StructRefArray의 원소 struct는 이 형질이 거짓이어야 하고(RMC2304), encode 고정이 이 형질에 기댄다. 생성
    // static_assert가 C++ 멤버 타입과 대조한다. import struct는 descriptor flags에서 온다(LinkImported가 대조한다).
    public bool ContainsReferenceArray { get; set; }

    // struct 중첩 깊이다: 1 + 멤버 struct·원소 struct 깊이의 최댓값(없으면 1). StructRules.MaxDepth 이하(RMC2303).
    // NATIVE 멤버는 저장 Record에 들어가지 않으므로 세지 않는다.
    public int Depth { get; set; }

    // 이 모듈 struct의 생성·등록 순위다: 1 + (PROPERTY struct 멤버·struct 원소 배열 멤버와 NATIVE 리플렉션 struct
    // 멤버 중 이 모듈 struct의 순위) 최댓값. 생성 특수화가 쓰는 모든 struct의 GeneratedType이 먼저 나오도록(StructRefArray
    // 원소의 GeneratedType<S>::Trace가 인스턴스화 시점보다 먼저 보이도록) 이 순위·이름 순으로 정렬한다.
    public int Rank { get; set; }

    // struct가 Migrate라는 이름의 멤버(메서드·함수 템플릿, 접근 무관)를 선언했다. 생성 Ops가 `&S::Migrate`를
    // RuntimeStructOps::migrate에 대입한다(시그니처·noexcept가 다르면 컴파일 오류 — 조용히 빠지지 않는다).
    public bool HasMigrate { get; set; }

    // 첫 Migrate 선언 위치다(진단용).
    public SourceSite? MigrateSite { get; set; }

    // Binds="…" 지정자의 원문 철자다(이 모듈 struct만, 없으면 ""). Clang 두 번째 실행이 정규 철자로 푼다.
    public string BindsSpelling { get; init; } = "";

    public SourceSite? BindsSite { get; init; }

    // Binds 미러 선언이 결합하는 실제 C++ 타입의 Clang 정규 철자다(예: rocks::Vector3<float>, 없으면 "").
    // descriptor의 binds 키다(Docs/Design/ReflectionFoundations.ko.md 4.5절).
    public string Binds { get; set; } = "";

    // 생성 코드가 쓰는 C++ 타입 이름이다: Binds 미러면 실제 타입의 정규 철자, 아니면 정규 이름. 스키마 이름(Name)은
    // 미러 이름 그대로다.
    public string NativeName => Binds.Length != 0 ? Binds : Name;

    // 선언이 있는 입력 헤더의 include 철자다(import 타입은 descriptor의 header).
    public string Header { get; set; } = "";

    public bool Private { get; set; }

    // 다른 모듈 descriptor에서 온 타입이다. 생성·inventory 대상이 아니다.
    public bool Imported { get; init; }

    public string Module { get; init; } = "";

    public List<FieldModel> OwnFields { get; } = [];

    public List<FieldModel> Fields { get; set; } = [];

    public bool IsEnum { get; init; }

    // enum underlying의 ValueKind 이름(UInt8·UInt16·UInt32·Int32)이다.
    public string Underlying { get; init; } = "";

    // EnumFlags::Flags(비트 플래그 enum)다.
    public bool FlagsEnum { get; init; }

    public string ToolPrefix { get; init; } = "";

    public List<EnumeratorModel> Enumerators { get; init; } = [];
}

// Classes는 이 모듈 타입(클래스·struct·enum)만, Imported는 import한 타입(정규 이름 → 모델)이다. Ordered는 이 모듈
// 타입의 생성·등록 순서다(enum, struct는 쓰는 struct 먼저 — Rank·이름 순, 클래스는 base 먼저).
internal sealed record SchemaModel(IReadOnlyDictionary<string, ClassModel> Classes, IReadOnlyList<ClassModel> Ordered,
    IReadOnlyDictionary<string, ClassModel> Imported)
{
    public ClassModel Find(string name) =>
        Classes.TryGetValue(name, out ClassModel? own) ? own : Imported[name];
}
