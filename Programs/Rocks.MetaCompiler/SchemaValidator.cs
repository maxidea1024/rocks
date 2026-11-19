namespace Rocks.MetaCompiler;

internal static partial class SchemaValidator
{
    // Clang이 desugar한 단일 값 철자 → 값 종류다. int8·int16은 signed char·short다(Docs/Design/ReflectionExtensions.ko.md 2절).
    // 평범한 char는 부호가 구현 정의라 넣지 않는다(UnsupportedFieldHint가 int8·uint8을 안내한다). rocks::Name은 리플렉션 B9c2의
    // Name 값 종류다(같은 문서 4절 — wire는 표시 문자열).
    private static readonly Dictionary<string, string> Scalars = new(StringComparer.Ordinal)
    {
        ["bool"] = "Bool",
        ["unsigned char"] = "UInt8",
        ["unsigned short"] = "UInt16",
        ["unsigned int"] = "UInt32",
        ["unsigned long long"] = "UInt64",
        ["signed char"] = "Int8",
        ["short"] = "Int16",
        ["int"] = "Int32",
        ["long long"] = "Int64",
        ["float"] = "Float32",
        ["double"] = "Float64",
        ["rocks::Guid"] = "Guid",
        ["rocks::String"] = "Utf8String",
        ["rocks::Name"] = "Name",
        ["rocks::AssetId"] = "AssetId",
        ["rocks::Sha256Digest"] = "Digest256",
        // InstancedStruct<>(리플렉션 B9j — Docs/Design/ReflectionExtensions.ko.md 11절)는 대상 없는 값 종류로 푼다(Clang은 기본 인자를
        // 생략한 `<>`로 내고, 명시한 void는 `<void>`로 낸다). InstancedStruct<Base>는 리플렉션 B9k가 연다.
        ["rocks::InstancedStruct<>"] = "InstancedStruct",
        ["rocks::InstancedStruct<void>"] = "InstancedStruct",
        // rocks::Text(리플렉션 B9l — Docs/Design/ReflectionExtensions.ko.md 13절)는 대상 없는 값 종류다.
        ["rocks::Text"] = "Text"
    };

    private static readonly string[] TargetKinds = ["Enum", "Struct", "ObjectReference", "WeakObjectReference", "SoftObjectReference", "ClassReference"];

    // 원본 선언의 철자 해석(ResolveValue·ResolveArray)이 만들 수 있는 단일 값 종류다: 스칼라 철자(Name 포함), 리플렉션 enum·struct,
    // 강한·weak·소프트 참조, 클래스 참조(ClassRef). ToolTests가 이 집합과 성질 표(ValueKindTable)의 Array 원소 열이 같은지 본다(새 종류는 철자 해석과 표에 함께 더한다).
    internal static IReadOnlyList<string> SourceValueKinds { get; } = [.. Scalars.Values.Distinct(StringComparer.Ordinal), .. TargetKinds];

    // "prefix<T>" 철자의 T를 돌려준다(아니면 null).
    private static string? TemplateTarget(string spelling, string prefix) =>
        spelling.StartsWith(prefix, StringComparison.Ordinal) && spelling.EndsWith('>') ? spelling[prefix.Length..^1].Trim() : null;

    // 강한 참조(ObjectRef<T>)·weak 참조(WeakObjectRef<T> — 리플렉션 B9d1, Docs/Design/ReflectionExtensions.ko.md 5.1절)·소프트 참조
    // (SoftObjectRef<T>)·클래스 참조(ClassRef<Base> — 리플렉션 B9d2, 같은 문서 5.2절) 철자면 대상 T와 값 종류를 돌려준다(아니면 null).
    private static (string Target, string Kind)? ReferenceSpelling(string spelling)
    {
        if (TemplateTarget(spelling, "rocks::ObjectRef<") is string strong)
        {
            return (strong, "ObjectReference");
        }

        if (TemplateTarget(spelling, "rocks::WeakObjectRef<") is string weak)
        {
            return (weak, "WeakObjectReference");
        }

        if (TemplateTarget(spelling, "rocks::SoftObjectRef<") is string soft)
        {
            return (soft, "SoftObjectReference");
        }

        if (TemplateTarget(spelling, "rocks::ClassRef<") is string classBase)
        {
            return (classBase, "ClassReference");
        }

        return null;
    }

    // base·참조 대상은 이 모듈 타입 → import 타입 순으로 정규 이름을 찾는다. 다른 모듈의 private 타입은
    // 쓸 수 없다(RMC2006). base·참조 대상은 클래스여야 하고, Enum·Struct 필드의 타입은 리플렉션 enum·struct여야
    // 한다. import 타입은 LinkImported로 이미 이어져 있다.
    public static SchemaModel Validate(Dictionary<string, ClassModel> classes, IReadOnlyDictionary<string, ClassModel> imported)
    {
        string Resolve(string spelling, string owner, SourceSite site)
        {
            string resolved = ResolveType(spelling, owner, site);
            if (resolved != "rocks::Object" && (Find(resolved).IsEnum || Find(resolved).IsStruct))
            {
                string what = Find(resolved).IsEnum ? "enum" : "struct";
                throw new MetadataException($"{owner}: base/reference type '{resolved}' is a reflected {what}, not a class", "RMC2003",
                    "Use a reflected class as the base or reference target; enums and structs are values (hold a struct as a field instead).").At(site);
            }

            return resolved;
        }

        // Binds 실제 타입의 정규 철자 → 미러다(이 모듈과 import). 한 실제 타입은 미러 하나만 묶고(RMC2004), 리플렉션
        // 타입·미러를 묶을 수 없다(RMC2004).
        Dictionary<string, ClassModel> mirrors = new(StringComparer.Ordinal);
        foreach (ClassModel mirror in imported.Values.Where(model => model.Binds.Length != 0))
        {
            mirrors.TryAdd(mirror.Binds, mirror);
        }

        foreach (ClassModel mirror in classes.Values.Where(model => model.Binds.Length != 0).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            if (classes.ContainsKey(mirror.Binds) || imported.ContainsKey(mirror.Binds))
            {
                throw new MetadataException($"{mirror.Name}: Binds target {mirror.Binds} is itself a reflected type", "RMC2004",
                    "Bind only non-reflected Core value types; reflected types already have their own metadata.").At(mirror.BindsSite ?? mirror.Site);
            }

            if (!mirrors.TryAdd(mirror.Binds, mirror))
            {
                throw new MetadataException($"{mirror.Name}: {mirrors[mirror.Binds].Name} already binds {mirror.Binds}", "RMC2004",
                    "Keep exactly one Binds mirror per real type; aliases such as rocks::Vec3 and rocks::Vector3<float> name the same type.").At(mirror.BindsSite ?? mirror.Site);
            }

            // 미러의 멤버는 실제 타입의 공개 데이터 멤버와 1:1이다(생성 static_assert가 이름·형식·개수·순서를 대조한다).
            // NATIVE 멤버·Migrate는 실제 타입에 둘 수 없으므로 받지 않는다(그 멤버 위치로 보고한다).
            // enum 미러(CurveData CV9)는 열거자를 실제 enum의 같은 이름·값으로 나열한다(생성 static_assert가 대조한다).
            if (mirror.IsEnum)
            {
                continue;
            }

            (string? invalid, SourceSite site) = mirror.NativeMembers.Count != 0
                ? ("a Binds mirror lists only persistent members (ROCKS_NATIVE is not allowed)", mirror.NativeMembers[0].Site)
                : mirror.HasMigrate ? ("a Binds mirror cannot declare Migrate (the real type owns its layout)", mirror.MigrateSite ?? mirror.Site)
                : mirror.OwnFields.Count == 0 ? ("a Binds mirror needs the real type's public data members as ROCKS_PROPERTY fields", mirror.Site)
                : ((string?)null, mirror.Site);
            if (invalid is not null)
            {
                throw new MetadataException($"{mirror.Name}: {invalid}",
                    hint: "Mirror every public data member of the real type, in declaration order, with ROCKS_PROPERTY and the same type.").At(site);
            }
        }

        // 리플렉션 enum(structs면 struct) 철자(desugar된 정규 이름)면 그 모델을 돌려준다(아니면 null). struct는 Binds
        // 실제 타입의 정규 철자로도 미러를 찾고, 미러 타입을 직접 쓰면 거부한다. 다른 모듈의 private 타입은 RMC2006이다.
        ClassModel? ValueType(string spelling, bool structs, string owner, SourceSite site)
        {
            string name = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
            if ((classes.TryGetValue(name, out ClassModel? named) || imported.TryGetValue(name, out named)) && named.Binds.Length != 0 &&
                named.IsEnum == !structs)
            {
                throw new MetadataException($"{owner}: '{name}' is a Binds mirror declaration; use the real type {named.Binds}", "RMC2000",
                    $"Declare the field with the real type ({named.Binds}); the mirror only describes it to the metadata compiler.").At(site);
            }

            if (!classes.ContainsKey(name) && !imported.ContainsKey(name) && mirrors.TryGetValue(name, out ClassModel? mirror) &&
                mirror.IsEnum == !structs)
            {
                name = mirror.Name;
            }

            if (classes.TryGetValue(name, out ClassModel? own))
            {
                return (structs ? own.IsStruct : own.IsEnum) ? own : null;
            }

            if (!imported.TryGetValue(name, out ClassModel? external) || (structs ? !external.IsStruct : !external.IsEnum))
            {
                return null;
            }

            if (external.Private)
            {
                throw new MetadataException($"{owner}: '{name}' is a private reflected type of module {external.Module}", "RMC2006",
                    "Move the type to a public header of its module or keep the reference inside that module.").At(site);
            }

            return external;
        }

        string ResolveType(string spelling, string owner, SourceSite site)
        {
            spelling = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
            string? Candidate(string name)
            {
                if (classes.ContainsKey(name))
                {
                    return name;
                }

                if (!imported.TryGetValue(name, out ClassModel? external))
                {
                    return null;
                }

                if (external.Private)
                {
                    throw new MetadataException($"{owner}: '{name}' is a private reflected type of module {external.Module}", "RMC2006",
                        "Move the type to a public header of its module or keep the reference inside that module.").At(site);
                }

                return name;
            }

            if (spelling == "rocks::Object")
            {
                return spelling;
            }

            if (Candidate(spelling) is string direct)
            {
                return direct;
            }

            string[] scope = owner.Split("::");
            for (int count = scope.Length - 1; count > 0; --count)
            {
                string candidate = string.Join("::", scope.Take(count)) + "::" + spelling;
                if (Candidate(candidate) is string found)
                {
                    return found;
                }
            }

            throw new MetadataException($"{owner}: base/reference type '{spelling}' is not reflected in this module or its imported dependencies",
                "RMC2003", "Reflect the base/reference type in this module or in a dependency module whose descriptor is imported.").At(site);
        }

        ClassModel Find(string name) => classes.TryGetValue(name, out ClassModel? own) ? own : imported[name];

        // 지원하지 않는 필드 형식의 안내다. Core 값 타입(Vector3·Color·Transform 등)은 CoreTypeBindings의 Binds 미러로
        // 리플렉션된다: 그 descriptor를 import했으면 같은 템플릿의 묶인 인스턴스를 알려 주고(예: Vector3<int>는 묶이지
        // 않았다), import하지 않았으면 그 모듈에 의존하라고 안내한다.
        string UnsupportedFieldHint(string spelling)
        {
            // 평범한 char는 signed char·unsigned char와 다른 형식이고 부호가 구현 정의라 받지 않는다(Docs/Design/ReflectionExtensions.ko.md 2절).
            if (spelling == "char")
            {
                return "Plain char has implementation-defined signedness; use rocks::int8 (signed char) or rocks::uint8 (unsigned char).";
            }

            static string Template(string text) => text.IndexOf('<') is int open and > 0 ? text[..open] : text;
            string[] bound = imported.Values.Where(model => model.Binds.Length != 0).Select(model => model.Binds).ToArray();
            string[] siblings = bound.Where(binds => Template(binds) == Template(spelling)).Order(StringComparer.Ordinal).ToArray();
            if (siblings.Length != 0)
            {
                return $"This instantiation is not bound by {StructRules.BindingModule}; use a bound type such as {string.Join(", ", siblings)}.";
            }

            if (bound.Length == 0 && StructRules.IsCoreValueType(spelling))
            {
                return $"Core value types are reflected by the {StructRules.BindingModule} module: depend on {StructRules.BindingModule} (add it to the module's RequiredModules) so its descriptor is imported.";
            }

            return "Use a supported scalar, Guid, String, Name, AssetId, Sha256Digest, ROCKS_ENUM enum, ROCKS_STRUCT struct, ObjectRef, WeakObjectRef, SoftObjectRef, ClassRef, rocks::InstancedStruct<>, rocks::Optional, rocks::OrderedMap/OrderedSet or rocks::Array/ObjectRefArray/StructRefArray field; raw pointers/other containers/cv-qualified fields need an explicit codec extension.";
        }

        // 단일 값 형식 철자 하나를 (값 종류, 대상 정규 이름, 대상 ID)로 푼다(필드와 Array<T> 원소가 같은 해석을 쓴다).
        // 지원하지 않는 형식이면 null이다.
        (string Kind, string? Reference, string? ReferenceId)? ResolveValue(string spelling, string owner, SourceSite site)
        {
            if (Scalars.TryGetValue(spelling, out string? kind))
            {
                return (kind, null, null);
            }

            if (ValueType(spelling, false, owner, site) is ClassModel enumeration)
            {
                return ("Enum", enumeration.Name, enumeration.Id);
            }

            if (ValueType(spelling, true, owner, site) is ClassModel structure)
            {
                return ("Struct", structure.Name, structure.Id);
            }

            if (ReferenceSpelling(spelling) is { } reference)
            {
                // 강한 참조와 weak·소프트 참조(GC 간선이 아님) 모두 대상이 리플렉션 클래스(또는 Object)여야 한다(아니면 RMC2003).
                string target = Resolve(reference.Target.Length == 0 ? "rocks::Object" : reference.Target, owner, site);
                // ClassRef의 Base는 리플렉션 클래스여야 한다 — 대상 ID가 필수라 Object 자체는 Base가 될 수 없다(RMC2003).
                if (reference.Kind == "ClassReference" && target == "rocks::Object")
                {
                    throw new MetadataException($"{owner}: ClassRef base must be a reflected class, not rocks::Object", "RMC2003",
                        "Use the most general reflected class the field accepts as the ClassRef base (rocks::ClassRef<MyBase>).").At(site);
                }

                return (reference.Kind, target, target == "rocks::Object" ? null : Find(target).Id);
            }

            // InstancedStruct<Base>(리플렉션 B9k): Base는 리플렉션 struct여야 한다(RMC2308 — 담을 수 있는 struct인지는 형질 계산
            // 뒤 CheckInstanced가 본다). InstancedStruct<>·<void>는 Scalars가 먼저 받는다.
            if (TemplateTarget(spelling, "rocks::InstancedStruct<") is string instancedBase)
            {
                if (ValueType(instancedBase, true, owner, site) is not ClassModel baseStruct)
                {
                    throw new MetadataException($"{owner}: InstancedStruct base '{instancedBase}' is not a reflected ROCKS_STRUCT", "RMC2308",
                        "Use rocks::InstancedStruct<> or name a reflected ROCKS_STRUCT as the base (rocks::InstancedStruct<RowBase>).").At(site);
                }

                return ("InstancedStruct", baseStruct.Name, baseStruct.Id);
            }

            return null;
        }

        // 배열 필드의 원소를 푼다(Docs/Design/ReflectionFoundations.ko.md 5.6절 MetaCompiler). 중첩 배열·컨테이너 인자의 배열은
        // RMC2304(ObjectRefArray·StructRefArray 인자는 RMC2003보다 먼저), cv 원소는 RMC2000, ObjectRefArray<T>의 T는 클래스
        // (기본 인자 생략 출력 ObjectRefArray<>는 rocks::Object — 아니면 RMC2003), StructRefArray<S>의 S는 리플렉션 struct
        // (아니면 RMC2304), Array<T>의 T는 단일 필드와 같은 해석(지원하지 않으면 원소 철자를 담은 RMC2000)이다. 원소
        // struct 형질에 따른 컨테이너 대조(RMC2304)는 형질을 계산한 뒤 CheckContainer가 한다.
        void ResolveArray(FieldModel field, string container, string element)
        {
            string symbol = $"{field.Owner}::{field.Name}";
            field.Kind = ArrayRules.Array;
            field.SpelledContainer = container;
            field.ElementSpelling = element;
            if (ArrayRules.IsContainer(element))
            {
                throw new MetadataException($"{symbol}: {container} element '{element}' is itself an array", "RMC2304", ArrayRules.ElementHint).At(field.Site);
            }

            // Map·Set(순서 없는 HashMap·HashSet 포함)은 배열 원소가 될 수 없다(컨테이너 쪽 코드 — Docs/Design/ReflectionExtensions.ko.md 15절).
            if (MapRules.IsContainer(element) || MapRules.IsUnordered(element))
            {
                throw new MetadataException($"{symbol}: {container} element '{element}' is a map or set", "RMC2304", ArrayRules.ElementHint).At(field.Site);
            }

            // Optional은 필드·멤버 자리에만 둔다(원소 위치는 컨테이너 쪽 코드 — Docs/Design/ReflectionExtensions.ko.md 15절).
            if (OptionalRules.Inner(element) is not null)
            {
                throw new MetadataException($"{symbol}: {container} element '{element}' is an Optional", "RMC2304",
                    "rocks::Optional<T> can only be a class field or struct member; store elements without Optional (a reference uses null, a ROCKS_STRUCT element can hold an Optional member).").At(field.Site);
            }

            if (ArrayRules.IsCvQualified(element))
            {
                throw new MetadataException($"{symbol}: cv-qualified array element '{element}' is unsupported",
                    hint: "Remove const/volatile from the element type; array elements are plain values.").At(field.Site);
            }

            if (container == ArrayRules.ObjectRefArray)
            {
                string target = Resolve(element.Length == 0 ? "rocks::Object" : element, field.Owner, field.Site);
                field.ElementKind = "ObjectReference";
                field.Reference = target;
                field.ReferenceId = target == "rocks::Object" ? null : Find(target).Id;
                return;
            }

            if (container == ArrayRules.StructRefArray)
            {
                ClassModel? structure = ValueType(element, true, field.Owner, field.Site);
                if (structure is null)
                {
                    throw new MetadataException($"{symbol}: StructRefArray element '{element}' is not a reflected ROCKS_STRUCT", "RMC2304",
                        "StructRefArray<S> holds reflected reference structs (ROCKS_STRUCT with ObjectRef members); " + ArrayRules.ElementHint).At(field.Site);
                }

                field.ElementKind = "Struct";
                field.Reference = structure.Name;
                field.ReferenceId = structure.Id;
                return;
            }

            if (ResolveValue(element, field.Owner, field.Site) is not { } value)
            {
                throw new MetadataException($"{symbol}: unsupported array element type '{element}'",
                    hint: UnsupportedFieldHint(element) + " If rocks::Array, ObjectRefArray or StructRefArray is explicitly instantiated (template class / extern template), Clang may print the written element spelling; remove the explicit instantiation.").At(field.Site);
            }

            field.ElementKind = value.Kind;
            field.Reference = value.Reference;
            field.ReferenceId = value.ReferenceId;
        }

        // 정적 배열 필드의 원소를 푼다(리플렉션 B9g — Docs/Design/ReflectionExtensions.ko.md 8·15절). 원소는 단일 필드와 같은 해석(ObjectRef<T>도
        // 그대로 — 원소가 소유 저장소 안이라 Registry 컨테이너가 필요 없다)이고 위치 규칙은 모두 RMC2307이다: N이 1..65535 밖, 다차원(원소가
        // 다시 C 배열), 컨테이너(배열·Map·Set·Optional) 원소, cv 원소, 지원하지 않는 원소 형식, 성질 표의 정적 배열 원소가 아닌 종류. Registry
        // 컨테이너를 담은 원소 struct는 struct 형질을 계산한 뒤 CheckStaticArray가 RMC2307로 거부한다.
        void ResolveStaticArray(FieldModel field, string element, string bound)
        {
            string symbol = $"{field.Owner}::{field.Name}";
            field.Kind = StaticArrayRules.StaticArray;
            field.ElementSpelling = element;
            if (!uint.TryParse(bound, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out uint count) ||
                count < 1 || count > StaticArrayRules.MaxStaticArrayElements)
            {
                throw new MetadataException($"{symbol}: static array size [{bound}] is outside 1..{StaticArrayRules.MaxStaticArrayElements}", "RMC2307",
                    StaticArrayRules.Hint).At(field.Site);
            }

            field.MaxElements = count;
            string? reason = StaticArrayRules.Split(element) is not null ? "is itself an array (multi-dimensional arrays are not reflected)"
                : ArrayRules.IsContainer(element) || MapRules.IsContainer(element) || MapRules.IsUnordered(element) || MapRules.SplitObjectRefMap(element) is not null
                    ? "is a container (containers do not nest)"
                : OptionalRules.Inner(element) is not null ? "is an Optional"
                : ArrayRules.IsCvQualified(element) ? "is cv-qualified" : null;
            if (reason is not null)
            {
                throw new MetadataException($"{symbol}: static array element '{element}' {reason}", "RMC2307", StaticArrayRules.Hint).At(field.Site);
            }

            if (ResolveValue(element, field.Owner, field.Site) is not { } value)
            {
                throw new MetadataException($"{symbol}: unsupported static array element type '{element}'", "RMC2307",
                    StaticArrayRules.Hint + " " + UnsupportedFieldHint(element)).At(field.Site);
            }

            if (!ValueKindTable.Has(value.Kind, ValueKindProperty.StaticArrayElement))
            {
                throw new MetadataException($"{symbol}: {value.Kind} cannot be a static array element", "RMC2307", StaticArrayRules.Hint).At(field.Site);
            }

            field.ElementKind = value.Kind;
            field.Reference = value.Reference;
            field.ReferenceId = value.ReferenceId;
        }

        // Optional 필드의 내부를 푼다(리플렉션 B9e — Docs/Design/ReflectionExtensions.ko.md 6·15절). 내부는 단일 필드와 같은 해석이고
        // 성질 표의 Optional 내부 종류여야 한다: 배열·중첩 Optional·참조(강한·weak·소프트·클래스)·지원하지 않는 형식·cv 내부는 RMC2306이다.
        // 참조 struct 내부는 struct 형질을 계산한 뒤 CheckOptional이 RMC2306으로 거부한다.
        void ResolveOptional(FieldModel field, string inner)
        {
            string symbol = $"{field.Owner}::{field.Name}";
            field.Kind = OptionalRules.Optional;
            field.ElementSpelling = inner;
            string? reason = ArrayRules.IsContainer(inner) ? "is an array"
                : MapRules.IsContainer(inner) || MapRules.IsUnordered(inner) ? "is a map or set"
                : OptionalRules.Inner(inner) is not null ? "is itself an Optional (Optionals do not nest)"
                : ArrayRules.IsCvQualified(inner) ? "is cv-qualified" : null;
            if (reason is not null)
            {
                throw new MetadataException($"{symbol}: Optional inner type '{inner}' {reason}", "RMC2306", OptionalRules.InnerHint).At(field.Site);
            }

            if (ResolveValue(inner, field.Owner, field.Site) is not { } value)
            {
                throw new MetadataException($"{symbol}: unsupported Optional inner type '{inner}'", "RMC2306",
                    OptionalRules.InnerHint + " " + UnsupportedFieldHint(inner)).At(field.Site);
            }

            if (!ValueKindTable.Has(value.Kind, ValueKindProperty.OptionalInner))
            {
                throw new MetadataException($"{symbol}: {value.Kind} cannot be an Optional inner type", "RMC2306", OptionalRules.InnerHint).At(field.Site);
            }

            field.ElementKind = value.Kind;
            field.Reference = value.Reference;
            field.ReferenceId = value.ReferenceId;
        }

        // Map·Set 필드의 키·값·원소를 푼다(리플렉션 B9f2 — Docs/Design/ReflectionExtensions.ko.md 7.2·15절). 키·원소·값은 단일 필드와
        // 같은 해석이고 위치 규칙은 모두 RMC2305다: 컨테이너(배열·Map·Set·Optional — 직접 중첩)·cv·지원하지 않는 형식, 성질 표의 Map 키·
        // Set 원소가 아닌 키·원소(실수·참조·struct), Map 값이 아닌 값, OrderedMap의 강한 참조 값(ObjectRefMap에 담는다), 기본이 아닌
        // 해시·같음 정책. 참조 struct 값은 struct 형질을 계산한 뒤 CheckMap이 RMC2305로 거부한다. objectTarget이 있으면
        // ObjectRefMap<K, T>(리플렉션 B9f4)이고 값은 objectClass(호출자가 푼 T — 빈 철자는 rocks::Object)의 강한 참조다.
        void ResolveMap(FieldModel field, string container, string key, string? value, string? policy, string? objectTarget = null, string? objectClass = null)
        {
            string symbol = $"{field.Owner}::{field.Name}";
            bool set = container == MapRules.Set;
            field.Kind = container;
            field.KeySpelling = set ? "" : key;
            if (policy is not null)
            {
                throw new MetadataException($"{symbol}: {(set ? "OrderedSet" : "OrderedMap")} with a {policy} cannot be reflected", "RMC2305",
                    "Use the default hash and equality policies; reflected maps and sets hash keys with SeededHash and compare them with their key equality.").At(field.Site);
            }

            (string Kind, string? Reference, string? ReferenceId) Resolve(string spelling, string position, ValueKindProperty property)
            {
                string? reason = ArrayRules.IsContainer(spelling) || MapRules.IsContainer(spelling) || MapRules.IsUnordered(spelling) ? "is a container (containers do not nest)"
                    : OptionalRules.Inner(spelling) is not null ? "is an Optional"
                    : ArrayRules.IsCvQualified(spelling) ? "is cv-qualified" : null;
                if (reason is not null)
                {
                    throw new MetadataException($"{symbol}: {position} type '{spelling}' {reason}", "RMC2305", MapRules.Hint).At(field.Site);
                }

                if (ResolveValue(spelling, field.Owner, field.Site) is not { } resolved)
                {
                    throw new MetadataException($"{symbol}: unsupported {position} type '{spelling}'", "RMC2305",
                        MapRules.Hint + " " + UnsupportedFieldHint(spelling)).At(field.Site);
                }

                if (property == ValueKindProperty.MapValue && resolved.Kind == "ObjectReference")
                {
                    throw new MetadataException($"{symbol}: map value '{spelling}' is a strong object reference", "RMC2305",
                        "Hold object references by key in rocks::ObjectRefMap<K, T> (Registry container); rocks::OrderedMap values cannot be ObjectRef.").At(field.Site);
                }

                if (!ValueKindTable.Has(resolved.Kind, property))
                {
                    throw new MetadataException($"{symbol}: {resolved.Kind} cannot be a {position}", "RMC2305", MapRules.Hint).At(field.Site);
                }

                return resolved;
            }

            if (set)
            {
                field.ElementSpelling = key;
                (string kind, string? reference, string? referenceId) = Resolve(key, "set element", ValueKindProperty.SetElement);
                field.ElementKind = kind;
                field.Reference = reference;
                field.ReferenceId = referenceId;
                return;
            }

            (string keyKind, string? keyReference, string? keyReferenceId) = Resolve(key, "map key", ValueKindProperty.MapKey);
            if (objectTarget is not null)
            {
                string target = objectClass!;
                field.ElementSpelling = objectTarget;
                field.KeyKind = keyKind;
                field.KeyReference = keyReference;
                field.KeyReferenceId = keyReferenceId;
                field.ElementKind = "ObjectReference";
                field.Reference = target;
                field.ReferenceId = target == "rocks::Object" ? null : Find(target).Id;
                return;
            }

            field.ElementSpelling = value!;
            (string valueKind, string? valueReference, string? valueReferenceId) = Resolve(value!, "map value", ValueKindProperty.MapValue);
            field.KeyKind = keyKind;
            field.KeyReference = keyReference;
            field.KeyReferenceId = keyReferenceId;
            field.ElementKind = valueKind;
            field.Reference = valueReference;
            field.ReferenceId = valueReferenceId;
        }

        // struct base 철자를 리플렉션 struct(이 모듈·import — 둘러싼 이름공간 순으로 찾는다) 하나와 필드 없는 비리플렉션 base로
        // 나눈다(리플렉션 B9k). 리플렉션 base가 둘 이상이거나 Binds 미러면 RMC2301이다.
        ClassModel? StructBase(string spelling, string owner, SourceSite site)
        {
            string name = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
            if (ValueType(name, true, owner, site) is ClassModel direct)
            {
                return direct;
            }

            string[] scope = owner.Split("::");
            for (int count = scope.Length - 1; count > 0; --count)
            {
                if (ValueType(string.Join("::", scope.Take(count)) + "::" + name, true, owner, site) is ClassModel found)
                {
                    return found;
                }
            }

            return null;
        }

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum))
        {
            if (!model.IsStruct)
            {
                model.Base = Resolve(model.Base, model.Name, model.Site);
            }
            else
            {
                foreach (string spelling in model.StructBaseSpellings)
                {
                    ClassModel? reflected;
                    try
                    {
                        reflected = StructBase(spelling, model.Name, model.Site);
                    }
                    catch (MetadataException) when (spelling.Length != 0)
                    {
                        throw new MetadataException($"{model.Name}: base {spelling} is a Binds mirror", "RMC2301",
                            "A Binds mirror cannot be a base; derive from a reflected ROCKS_STRUCT or hold the bound value as a field.").At(model.Site);
                    }

                    if (reflected is null)
                    {
                        // 리플렉션 클래스(Object 파생)는 struct의 base가 될 수 없다.
                        string bare = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
                        if (bare == "rocks::Object" || classes.GetValueOrDefault(bare) is { IsStruct: false, IsEnum: false } ||
                            imported.GetValueOrDefault(bare) is { IsStruct: false, IsEnum: false })
                        {
                            throw new MetadataException($"{model.Name}: a ROCKS_STRUCT cannot derive from reflected class {bare}", "RMC2301",
                                "Structs are plain values; derive from a reflected ROCKS_STRUCT or hold an ObjectRef to the class instead.").At(model.Site);
                        }

                        model.EmptyBases.Add(spelling);
                        continue;
                    }

                    if (reflected.Binds.Length != 0)
                    {
                        throw new MetadataException($"{model.Name}: base {reflected.Name} is a Binds mirror", "RMC2301",
                            "A Binds mirror cannot be a base; derive from a reflected ROCKS_STRUCT or hold the bound value as a field.").At(model.Site);
                    }

                    if (model.Base.Length != 0)
                    {
                        throw new MetadataException($"{model.Name}: a ROCKS_STRUCT can have only one reflected base ({model.Base}, {reflected.Name})", "RMC2301",
                            "Derive from one reflected ROCKS_STRUCT and hold the other as a nested field.").At(model.Site);
                    }

                    model.Base = reflected.Name;
                }
            }

            foreach (FieldModel field in model.OwnFields)
            {
                if (MapRules.IsUnordered(field.NativeSpelling))
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: unordered container '{field.NativeSpelling}' cannot be reflected", "RMC2305",
                        "Use rocks::OrderedMap<K, V> or rocks::OrderedSet<T>; HashMap and HashSet iterate in hash order, which is not a stable saved state.").At(field.Site);
                }

                if (StaticArrayRules.Split(field.CanonicalSpelling ?? field.NativeSpelling) is (string fixedElement, string bound))
                {
                    ResolveStaticArray(field, fixedElement, bound);
                }
                else if (OptionalRules.Inner(field.NativeSpelling) is string inner)
                {
                    ResolveOptional(field, inner);
                }
                else if (MapRules.SplitObjectRefMap(field.NativeSpelling) is (string refKey, string refTarget))
                {
                    // ObjectRefMap<K, T>의 T는 ObjectRefArray<T>와 같이 클래스다(빈 철자는 rocks::Object — 아니면 RMC2003).
                    ResolveMap(field, MapRules.Map, refKey, null, null, refTarget, Resolve(refTarget.Length == 0 ? "rocks::Object" : refTarget, field.Owner, field.Site));
                }
                else if (MapRules.Split(field.NativeSpelling) is (string mapContainer, string key, var mapValue, var policy))
                {
                    ResolveMap(field, mapContainer, key, mapValue, policy);
                }
                else if (ArrayRules.Split(field.NativeSpelling) is (string container, string element))
                {
                    ResolveArray(field, container, element);
                }
                else if (ResolveValue(field.NativeSpelling, field.Owner, field.Site) is { } value)
                {
                    field.Kind = value.Kind;
                    field.Reference = value.Reference;
                    field.ReferenceId = value.ReferenceId;
                }
                else
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: unsupported field type '{field.NativeSpelling}'",
                    hint: UnsupportedFieldHint(field.NativeSpelling)).At(field.Site);
                }

                // 철자 해석이 정한 종류가 성질 표에서 그 자리(배열이면 원소, 그리고 클래스 필드·struct 멤버)에 허용되는지 본다. 해석이
                // 만드는 종류는 지금 모두 표가 허용하므로 늘 통과한다 — 새 종류를 더할 때 철자 해석과 표가 갈라지지 않게 하는 확인이다.
                if (field.IsArray && !ValueKindTable.Has(field.ElementKind!, ValueKindProperty.ArrayElement))
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: {field.ElementKind} cannot be an array element", "RMC2304",
                        ArrayRules.ElementHint).At(field.Site);
                }

                if (!ValueKindTable.Has(field.Kind, model.IsStruct ? ValueKindProperty.StructMember : ValueKindProperty.Field))
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: value kind {field.Kind} cannot be a {(model.IsStruct ? "struct member" : "class field")}",
                        hint: UnsupportedFieldHint(field.NativeSpelling)).At(field.Site);
                }

                // 지정자를 그 필드 종류에 쓸 수 있는지 본다(RMC2202 — 종류를 먼저 판정하고 값 범위는 그 뒤에 본다).
                if (field.Spec.Required && field.IsOptional)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: Required cannot apply to an Optional field", "RMC2202",
                        "Remove Required; an unset Optional is a valid value (use a non-Optional field when the value must always exist).").At(field.Site);
                }

                if (field.Spec.Required && !field.AcceptsRequired)
                {
                    // WeakObjectRef는 대상이 죽으면 저절로 null이 되므로 Required를 지킬 수 없다(Docs/Design/ReflectionExtensions.ko.md 5.1절). Map은 값마다,
                    // Set은 원소마다 non-null이다(키에는 Required가 없다 — 리플렉션 B9f2).
                    throw new MetadataException($"{field.Owner}::{field.Name}: Required applies only to object, soft object and class reference fields, arrays, static arrays and sets of them and maps with such values", "RMC2202",
                        "Remove Required or change the field to ObjectRef, SoftObjectRef, ClassRef, ObjectRefArray, ObjectRefMap, Array<SoftObjectRef>, Array<ClassRef>, a static array of ObjectRef/SoftObjectRef/ClassRef, OrderedSet<SoftObjectRef/ClassRef> or OrderedMap<K, SoftObjectRef/ClassRef>; a WeakObjectRef becomes null when its target dies, so it cannot be Required.").At(field.Site);
                }

                // Required와 복제 제외 플래그는 함께 쓸 수 없다(리플렉션 B9h1 — 복제본이 늘 null을 가져 패키지 저장의 rejectMissingRequired가
                // 실패한다).
                if (field.Spec.Required && (field.Spec.DuplicateTransient || field.Spec.NonTryoutDuplicateTransient))
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: Required cannot be combined with DuplicateTransient or NonTryoutDuplicateTransient", "RMC2202",
                        "A duplicate never carries this field, so a Required check on the duplicate would always fail; remove Required or the duplicate-transient specifier.").At(field.Site);
                }

                if (!field.IsCounted && field.Spec.MaxElements != 0)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: MaxElements applies only to array, map and set fields", "RMC2202",
                        "Remove MaxElements; it limits the element count of reflected array, map and set fields (a static array T name[N] always holds exactly N elements).").At(field.Site);
                }

                CheckSettingsField(model, field);
                if (field.IsCounted)
                {
                    // 0과 2³¹ 이상은 지정자 문법(RMC2000)이 이미 막는다.
                    field.MaxElements = field.Spec.MaxElements == 0 ? ArrayRules.DefaultMaxElements : field.Spec.MaxElements;
                    if (field.MaxElements > ArrayRules.MaxArrayElements)
                    {
                        throw new MetadataException($"{field.Owner}::{field.Name}: MaxElements={field.MaxElements} exceeds {ArrayRules.MaxArrayElements} (MaxArrayElements)",
                            hint: $"Use MaxElements between 1 and {ArrayRules.MaxArrayElements}; large byte or numeric data belongs in bulk data.").At(field.Site);
                    }
                }
            }

            // NATIVE 멤버의 직접 타입이 리플렉션 struct면 생성 코드가 그 struct의 형질로 RMC2205·2206을 한 번 더 확인한다.
            foreach (NativeMemberModel native in model.NativeMembers)
            {
                // C 배열 멤버는 모든 차원을 벗긴 원소로 본다(StaticArrayProbe — `RefStruct cache[2]`도 원소 struct 형질로 확인한다).
                native.Struct = ValueType(ClangAstParser.CleanType(native.ElementSpelling ?? native.Spelling), true, model.Name, native.Site)?.Name;
            }
        }

        // 필드 목록은 상속 포함 안정 ID 순이다. 4096개 이하, 도구 이름은 대소문자 무시로 유일하다(RMC2203).
        List<FieldModel> BuildFields(ClassModel model, IEnumerable<FieldModel> inherited)
        {
            List<FieldModel> fields = inherited.Concat(model.OwnFields).OrderBy(field => field.Id, StringComparer.Ordinal).ToList();
            if (fields.Count > 4096)
            {
                throw new MetadataException($"{model.Name}: {(model.IsStruct ? "" : "inherited ")}field count exceeds 4096",
                    hint: "Split the type; a reflected class (including inherited fields) or struct has at most 4096 fields.").At(model.Site);
            }

            Dictionary<string, FieldModel> toolNames = new(StringComparer.OrdinalIgnoreCase);
            foreach (FieldModel field in fields)
            {
                if (!toolNames.TryAdd(field.EffectiveToolName, field))
                {
                    FieldModel other = toolNames[field.EffectiveToolName];
                    // import 필드의 위치는 descriptor이므로 이 모듈 쪽 필드 위치로 보고한다.
                    SourceSite site = model.OwnFields.Contains(field) ? field.Site : model.OwnFields.Contains(other) ? other.Site : model.Site;
                    throw new MetadataException($"{model.Name}: tool name '{field.EffectiveToolName}' of {field.Owner}::{field.Name} collides with {other.Owner}::{other.Name} (case-insensitive)",
                        "RMC2203", "Give the field a distinct Name=\"...\" specifier; tool names are unique per type including inherited fields.").At(site);
                }
            }

            return fields;
        }

        // 이 모듈 배열 필드의 C++ 철자 컨테이너가 원소 종류·원소 struct 형질로 정해지는 파생 컨테이너와 같아야 한다(RMC2304):
        // Array<ObjectRef<T>>는 ObjectRefArray<T>, 참조 struct 원소는 StructRefArray<S>(noexcept 이동을 직접 정의해 Clang의
        // Array static_assert를 지나는 Array<참조 struct>도 여기서 막는다), 참조 없는 struct 원소는 Array<S>다. StructRefArray의
        // 원소 struct는 Registry 컨테이너를 (중첩까지) 담지 않는다(원소 블록이 owner 저장소 밖이다).
        void CheckContainer(FieldModel field)
        {
            if (!field.IsArray || field.SpelledContainer is null)
            {
                return;
            }

            string symbol = $"{field.Owner}::{field.Name}";
            string derived = field.ContainerOf(Find)!;
            // 참조 struct 원소가 Registry 컨테이너를 담으면 어느 컨테이너로도 쓸 수 없으므로 철자 불일치보다 먼저 알린다(StructRefArray로
            // 바꾸라는 안내가 다음 실행에서 다시 실패하지 않게).
            if (derived == ArrayRules.StructRefArray && Find(field.Reference!).ContainsReferenceArray)
            {
                throw new MetadataException($"{symbol}: {field.SpelledContainer} element struct {field.Reference} holds ObjectRefArray, StructRefArray or ObjectRefMap members", "RMC2304",
                    "Keep reference containers out of array element structs (directly or nested); store them in the owning class instead.").At(field.Site);
            }

            if (derived != field.SpelledContainer)
            {
                string reason = derived switch
                {
                    ArrayRules.ObjectRefArray => $"object reference elements '{field.ElementSpelling}' need rocks::ObjectRefArray<T>",
                    ArrayRules.StructRefArray => $"element struct {field.Reference} holds strong object references and needs rocks::StructRefArray<S>",
                    _ => $"element struct {field.Reference} holds no strong object references and needs rocks::Array<S>"
                };
                throw new MetadataException($"{symbol}: {field.SpelledContainer} element '{field.ElementSpelling}' cannot be used: {reason}", "RMC2304",
                    ArrayRules.ElementHint).At(field.Site);
            }
        }

        // Optional<S>의 S는 값 struct다(강한 참조를 담는 struct는 null이 이미 "없음"이고 Optional 안의 간선은 GC·관찰자 계수가 따라가지
        // 않는다 — RMC2306, 런타임 등록도 거부한다). S의 형질을 계산한 뒤에 본다.
        void CheckOptional(FieldModel field)
        {
            if (field.IsOptional && field.StructTarget && Find(field.Reference!).ContainsStrongReference)
            {
                throw new MetadataException($"{field.Owner}::{field.Name}: Optional inner struct {field.Reference} holds strong object references", "RMC2306",
                    OptionalRules.InnerHint).At(field.Site);
            }
        }

        // 정적 배열의 원소 struct는 Registry 컨테이너(ObjectRefArray·StructRefArray·ObjectRefMap — 중첩 포함)를 담지 않는다(RMC2307 — 런타임
        // 등록도 거부한다, Docs/Design/ReflectionExtensions.ko.md 8절 "구현(B9g)에서 정한 점"). 참조 struct 원소는 받는다. 원소 struct의
        // 형질을 계산한 뒤에 본다.
        void CheckStaticArray(FieldModel field)
        {
            if (field.IsStaticArray && field.StructTarget && Find(field.Reference!).ContainsReferenceArray)
            {
                throw new MetadataException($"{field.Owner}::{field.Name}: static array element struct {field.Reference} holds ObjectRefArray, StructRefArray or ObjectRefMap members", "RMC2307",
                    "Keep reference containers out of static array element structs (directly or nested); store them in the owning class instead.").At(field.Site);
            }
        }

        // InstancedStruct<Base>의 Base(리플렉션 B9k)는 담을 수 있는 struct여야 하고(강한 참조·Registry 컨테이너·InstancedStruct·Required·
        // WorldOnly 소유자 요구 없음 — RMC2308) struct 멤버 자리면 1 + Base 깊이가 MaxStructDepth 이하여야 한다(RMC2303 — 그 struct가 최상위
        // 필드일 때의 위치 깊이 합, 더 깊은 자리는 런타임 깊이 합 규칙이 본다). Base의 형질을 계산한 뒤에 본다.
        void CheckInstanced(ClassModel owner, FieldModel field)
        {
            if (field.TargetKind != "InstancedStruct" || field.Reference is null)
            {
                return;
            }

            ClassModel baseStruct = Find(field.Reference);
            if (baseStruct.ContainsStrongReference || baseStruct.ContainsReferenceArray || ContainsInstancedStructTrait(baseStruct, Find) ||
                ContainsRequiredTrait(baseStruct, Find) || baseStruct.RequiresWorldOnlyOwner)
            {
                throw new MetadataException($"{field.Owner}::{field.Name}: InstancedStruct base {baseStruct.Name} cannot be held (strong references, reference containers, InstancedStruct or Required members)", "RMC2308",
                    "Use a plain value ROCKS_STRUCT as the InstancedStruct base.").At(field.Site);
            }

            if (owner.IsStruct && 1 + baseStruct.Depth > StructRules.MaxDepth)
            {
                throw new MetadataException($"{field.Owner}::{field.Name}: InstancedStruct base {baseStruct.Name} nesting depth {1 + baseStruct.Depth} exceeds {StructRules.MaxDepth}", "RMC2303",
                    "Flatten the base struct or hold the InstancedStruct in a class field.").At(field.Site);
            }
        }

        // Map 값 struct는 값 struct다(강한 참조를 담는 struct는 ObjectRefMap·인라인 struct 멤버로 둔다 — RMC2305, 런타임 등록도 거부한다).
        // 값 struct의 형질을 계산한 뒤에 본다.
        void CheckMap(FieldModel field)
        {
            if (field.IsMap && field.StructTarget && Find(field.Reference!).ContainsStrongReference)
            {
                throw new MetadataException($"{field.Owner}::{field.Name}: map value struct {field.Reference} holds strong object references", "RMC2305",
                    MapRules.Hint).At(field.Site);
            }
        }

        // struct: 필드 목록 → 순환·깊이(DFS, Order·생성 전 — PROPERTY struct 멤버·struct 원소 배열 멤버·Optional<struct> 멤버) → 순위
        // (PROPERTY·NATIVE struct 멤버와 원소·내부 struct) → 형질(쓰는 struct가 먼저 계산되도록 순위 순)·컨테이너 대조.
        ClassModel[] structs = classes.Values.Where(model => model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal).ToArray();
        // struct 상속(리플렉션 B9k)은 base 필드를 펼친다: base(이 모듈이면 먼저 만든다, import면 descriptor의 상속 포함 전체 필드)
        // 필드 + 자기 필드이고 Atomic은 base에서 상속된다.
        HashSet<string> builtStructs = new(StringComparer.Ordinal);
        HashSet<string> buildingStructs = new(StringComparer.Ordinal);
        void BuildStruct(ClassModel model)
        {
            if (builtStructs.Contains(model.Name))
            {
                return;
            }

            if (!buildingStructs.Add(model.Name))
            {
                throw new MetadataException($"{model.Name}: struct inheritance cycle", "RMC2301").At(model.Site);
            }

            List<FieldModel> inherited = [];
            if (model.Base.Length != 0)
            {
                ClassModel baseModel = classes.TryGetValue(model.Base, out ClassModel? ownBase) ? ownBase : imported[model.Base];
                if (ownBase is not null)
                {
                    BuildStruct(ownBase);
                }

                inherited.AddRange(baseModel.Fields);
                model.Atomic |= baseModel.Atomic;
            }

            model.Fields = BuildFields(model, inherited);
            buildingStructs.Remove(model.Name);
            builtStructs.Add(model.Name);
        }

        foreach (ClassModel model in structs)
        {
            BuildStruct(model);
        }

        ComputeDepths(structs, Find);
        ComputeRanks(structs, classes);
        ClassModel[] orderedStructs = structs.OrderBy(model => model.Rank).ThenBy(model => model.Name, StringComparer.Ordinal).ToArray();
        foreach (ClassModel model in orderedStructs)
        {
            // Transient 참조도 GC가 추적하므로 강한 참조로 센다. ObjectRefArray·ObjectRefMap 필드와 참조 struct 원소
            // 배열(StructRefArray)도 강한 참조다.
            model.ContainsStrongReference = model.Fields.Any(field => field.IsObjectReferenceFieldKind || (field.StructTarget && Find(field.Reference!).ContainsStrongReference));
            // NATIVE 멤버의 WorldOnly 소유자 형질은 struct에서는 오류가 아니라 형질로 전파된다(Docs/Design/
            // ReflectionFoundations.ko.md 2.4절). 원소 struct의 형질도 전파된다.
            model.RequiresWorldOnlyOwner = model.WorldOnlyNatives.Count != 0 ||
                model.Fields.Any(field => field.StructTarget && Find(field.Reference!).RequiresWorldOnlyOwner) ||
                model.NativeMembers.Any(native => native.Struct is not null && Find(native.Struct).RequiresWorldOnlyOwner) ||
                (model.Base.Length != 0 && Find(model.Base).RequiresWorldOnlyOwner); // base의 NATIVE 멤버 형질도 상속된다(리플렉션 B9k)
            model.ContainsReferenceArray = ContainsReferenceArray(model, Find);
            // 설정 struct는 값이다(설정 계층이 리플렉션 struct 값 대상으로 읽는다 — Docs/Design/ConfigSystem.ko.md 7절). 강한 참조를 담으면
            // 값 대상이 될 수 없다.
            if (model.IsSettings && (model.ContainsStrongReference || model.ContainsReferenceArray))
            {
                throw new MetadataException($"{model.Name}: a Settings struct cannot hold strong object references or reference arrays", "RMC2210",
                    "Settings structs are plain values read from config files; refer to assets by SoftObjectRef or AssetId instead.").At(model.Site);
            }

            // 검증 hook(Docs/Design/CurveData.ko.md 9.1절)은 순수 함수라 Registry를 읽지 못하고, 리플렉션 쓰기는 hook struct를 통째로
            // 복사해 검증한 뒤 게시한다. 그래서 hook struct는 참조 없는 값이어야 한다.
            if (model.HasValidate && (model.ContainsStrongReference || model.ContainsReferenceArray))
            {
                throw new MetadataException($"{model.Name}: a struct with a Validate hook cannot hold strong object references or reference arrays", "RMC2309",
                    "Keep validated structs plain values; validate reference-holding data where it is used instead.").At(model.ValidateSite ?? model.Site);
            }

            foreach (FieldModel field in model.OwnFields)
            {
                CheckOptional(field);
                CheckMap(field);
                CheckStaticArray(field);
                CheckContainer(field);
                CheckInstanced(model, field);
            }
        }

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum && !model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            foreach (FieldModel field in model.OwnFields)
            {
                CheckOptional(field);
                CheckMap(field);
                CheckStaticArray(field);
                CheckContainer(field);
                CheckInstanced(model, field);
            }
        }

        // name이 ancestor 자신이거나 base 사슬에 ancestor가 있는지다(이 모듈과 import 클래스 — Within 상속 판정, 리플렉션 B9h2).
        bool DerivesFrom(string name, string ancestor)
        {
            for (string? current = name; current is not null && current != "rocks::Object";)
            {
                if (current == ancestor)
                {
                    return true;
                }

                current = classes.TryGetValue(current, out ClassModel? own) ? own.Base : imported.GetValueOrDefault(current)?.Base;
            }

            return false;
        }

        List<ClassModel> ordered = [];
        HashSet<string> visiting = new(StringComparer.Ordinal);
        HashSet<string> visited = new(StringComparer.Ordinal);
        void Order(ClassModel model)
        {
            if (visited.Contains(model.Name))
            {
                return;
            }

            if (!visiting.Add(model.Name))
            {
                throw new MetadataException($"{model.Name}: inheritance cycle").At(model.Site);
            }

            List<FieldModel> inherited = [];
            if (model.Base != "rocks::Object")
            {
                ClassModel baseModel;
                if (classes.TryGetValue(model.Base, out ClassModel? ownBase))
                {
                    Order(ownBase);
                    baseModel = ownBase;
                }
                else
                {
                    baseModel = imported[model.Base]; // import 타입의 Fields는 상속 포함 전체다
                }

                inherited.AddRange(baseModel.Fields);
                // WorldOnly는 상속된다(파생에 다시 쓰지 않아도 된다).
                model.WorldOnly |= baseModel.WorldOnly;
                // Transient·Deprecated도 상속된다(끄는 지정자는 없다 — 리플렉션 B9h2).
                model.Transient |= baseModel.Transient;
                model.Deprecated |= baseModel.Deprecated;
                model.Within = baseModel.Within;
                model.WithinId = baseModel.WithinId;
            }

            // Within(리플렉션 B9h2): 지정하면 리플렉션 클래스여야 하고(RMC2003) base의 Within이 있으면 그 클래스 또는 파생이어야 한다
            // (RMC2208). 지정하지 않으면 base의 것을 이어받는다. 자기 자신을 Within으로 둘 수 있다(UE와 같다).
            if (model.WithinResolved is string within)
            {
                string resolved = within == model.Name ? within : Resolve(within, model.Name, model.Site);
                if (resolved == "rocks::Object")
                {
                    throw new MetadataException($"{model.Name}: Within must be a reflected class, not rocks::Object", "RMC2208",
                        "Remove Within (every Outer is an Object) or name the reflected class every Outer must be.").At(model.Site);
                }

                if (model.Within is string baseWithin && !DerivesFrom(resolved, baseWithin))
                {
                    throw new MetadataException($"{model.Name}: Within {resolved} must be {baseWithin} or derive from it (the base class Within)", "RMC2208",
                        "A derived class can only narrow the Within of its base; name the base Within class or one of its subclasses.").At(model.Site);
                }

                model.Within = resolved;
                model.WithinId = resolved == model.Name ? model.Id : Find(resolved).Id;
            }

            model.Fields = BuildFields(model, inherited);

            // WorldOnly 소유자 형질을 담는 NATIVE 멤버는 WorldOnly 클래스(상속 포함)에만 둔다(RMC2206).
            foreach ((string symbol, string path, SourceSite site) in model.WorldOnlyNatives)
            {
                if (!model.WorldOnly)
                {
                    throw new MetadataException($"{symbol}: NATIVE member requires a WorldOnly owner ({path})", "RMC2206",
                        "Mark the class ROCKS_CLASS(WorldOnly) or move the member to a World-side type.").At(site);
                }
            }

            // 그 형질을 가진 struct를 (상속 필드·원소 struct·NATIVE 멤버 포함) 담는 클래스도 WorldOnly여야 한다(RMC2206).
            FieldModel? worldOnlyField = model.WorldOnly ? null
                : model.Fields.FirstOrDefault(field => field.StructTarget && Find(field.Reference!).RequiresWorldOnlyOwner);
            if (worldOnlyField is not null)
            {
                throw new MetadataException($"{worldOnlyField.Owner}::{worldOnlyField.Name}: struct {worldOnlyField.Reference} requires a WorldOnly owner", "RMC2206",
                    "Mark the class ROCKS_CLASS(WorldOnly) or move the struct field to a World-side type.")
                    .At(model.OwnFields.Contains(worldOnlyField) ? worldOnlyField.Site : model.Site);
            }

            NativeMemberModel? worldOnlyNative = model.WorldOnly ? null
                : model.NativeMembers.FirstOrDefault(native => native.Struct is not null && Find(native.Struct).RequiresWorldOnlyOwner);
            if (worldOnlyNative is not null)
            {
                throw new MetadataException($"{model.Name}::{worldOnlyNative.Name}: NATIVE member struct {worldOnlyNative.Struct} requires a WorldOnly owner", "RMC2206",
                    "Mark the class ROCKS_CLASS(WorldOnly) or move the member to a World-side type.").At(worldOnlyNative.Site);
            }

            visiting.Remove(model.Name);
            visited.Add(model.Name);
            ordered.Add(model);
        }

        CheckTypeToolNames(classes, imported);

        // enum은 그 enum을 쓰는 타입보다, struct는 그 struct를 쓰는 struct·클래스보다 먼저 등록해야 하므로 앞에 둔다.
        // struct는 깊이 순(멤버 struct가 먼저)이고 같은 깊이는 이름 순이다.
        ordered.AddRange(classes.Values.Where(model => model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal));
        ordered.AddRange(orderedStructs);
        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum && !model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Order(model);
        }

        // Deprecated가 아닌 타입의 PROPERTY는 Deprecated 클래스를 가리키지 않는다(RMC2209 — UE도 막는다, 리플렉션 B9h2): 강한·weak·소프트
        // 참조와 ClassRef(그 컨테이너 원소·Map 값 포함)의 대상, Map 키의 Soft·ClassRef 대상. Deprecated 타입 자신의 필드와 NATIVE 멤버는
        // 보지 않는다. 상속 플래그가 정해진 뒤에 본다.
        bool IsDeprecatedClass(string? name) =>
            name is not null && name != "rocks::Object" && (classes.TryGetValue(name, out ClassModel? own) ? own : imported.GetValueOrDefault(name)) is { IsEnum: false, IsStruct: false, Deprecated: true };
        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum && !model.Deprecated).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            foreach (FieldModel field in model.OwnFields)
            {
                bool referenceTarget = field.TargetKind is "ObjectReference" or "WeakObjectReference" or "SoftObjectReference" or "ClassReference";
                string? deprecated = referenceTarget && IsDeprecatedClass(field.Reference) ? field.Reference
                    : field.KeyKind is "SoftObjectReference" or "ClassReference" && IsDeprecatedClass(field.KeyReference) ? field.KeyReference : null;
                if (deprecated is not null)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: refers to Deprecated class {deprecated}", "RMC2209",
                        "Retire this field (ids retire) or mark its owner Deprecated; move old data with Version and Migrate instead of referring to a Deprecated class.").At(field.Site);
                }
            }
        }

        return new(classes, ordered, imported);
    }

    // 콘솔 설정·설정 snapshot이 담을 수 있는 필드 종류다(Docs/Design/ConfigSystem.ko.md 5.1절 — bool·int32·int64·float·double·String·Name·
    // 리플렉션 enum). Optional·컨테이너·struct는 받지 않는다.
    private static readonly string[] ConsoleKinds = ["Bool", "Int32", "Int64", "Float32", "Float64", "Utf8String", "Name", "Enum"];

    // 설정 필드 지정자를 검사한다(Docs/Design/ConfigSystem.ko.md 7절): 설정 struct의 직접 멤버만 쓸 수 있고(RMC2210), ConsoleName·RenderRead는
    // 콘솔 설정 종류의 필드에만 쓴다(RMC2202). ConsoleName은 한 struct 안에서 유일하다(RMC2210 — 모듈·실행 사이 유일성은 설정 계층이 등록 때
    // 본다).
    private static void CheckSettingsField(ClassModel model, FieldModel field)
    {
        if (!field.Spec.HasSettings)
        {
            return;
        }

        if (!model.IsStruct || !model.IsSettings)
        {
            throw new MetadataException($"{field.Owner}::{field.Name}: settings specifiers (UserConfig, ConsoleName, RestartRequired, StartupOnly, RenderRead, DevelopmentOnly) apply only to members of a ROCKS_STRUCT(Settings(...))", "RMC2210",
                "Declare the owner as ROCKS_STRUCT(Settings(Branch = \"...\", Section = \"...\")) or remove the settings specifiers.").At(field.Site);
        }

        if ((field.Spec.ConsoleName.Length != 0 || field.Spec.RenderRead) && !ConsoleKinds.Contains(field.Kind, StringComparer.Ordinal))
        {
            throw new MetadataException($"{field.Owner}::{field.Name}: ConsoleName and RenderRead apply only to bool, int32, int64, float, double, String, Name and reflected enum fields", "RMC2202",
                "Expose a single value field; containers, Optional and structs cannot be console settings or snapshot values.").At(field.Site);
        }

        if (field.Spec.ConsoleName.Length != 0 &&
            model.OwnFields.Any(other => other != field && string.Equals(other.Spec.ConsoleName, field.Spec.ConsoleName, StringComparison.Ordinal)))
        {
            throw new MetadataException($"{field.Owner}::{field.Name}: ConsoleName \"{field.Spec.ConsoleName}\" is used by another field of {model.Name}", "RMC2210",
                "Give every console setting a distinct name; a console name maps to exactly one value.").At(field.Site);
        }
    }

    // 타입 도구 이름(클래스·struct — Docs/Design/ReflectionExtensions.ko.md 5.2절)을 검사한다(RMC2203): 실효 이름이 형식에 맞고
    // (none 예약 — 기본 이름이 none이면 Name 지정자가 필요하다), 이 모듈의 타입끼리와 이 모듈 타입·import 타입 사이에서 대소문자
    // 무시로 유일하다(런타임 TypeRegistry 등록이 AlreadyExists로 막는 충돌을 먼저 잡는다). import 타입끼리의 충돌은
    // CompilerDriver.LoadImports가 RMC2203으로 먼저 막는다.
    private static void CheckTypeToolNames(Dictionary<string, ClassModel> classes, IReadOnlyDictionary<string, ClassModel> imported)
    {
        const string hint = "Give one of the types a distinct ROCKS_CLASS(Name=\"...\") or ROCKS_STRUCT(Name=\"...\"); type tool names are unique across a module and its imports (case-insensitive).";
        Dictionary<string, ClassModel> names = new(StringComparer.OrdinalIgnoreCase);
        // 이 모듈이 같은 정규 이름을 다시 선언한 import 타입은 뺀다(그 재정의는 CompilerDriver가 RMC2004로 알린다).
        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum && !classes.ContainsKey(model.Name))
            .OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            names.TryAdd(external.EffectiveToolName, external);
        }

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            string toolName = model.EffectiveToolName;
            if (!TypeToolName.IsValid(toolName))
            {
                throw new MetadataException($"{model.Name}: type tool name '{toolName}' is not a valid tool name ('none' is reserved)", "RMC2203",
                    "Rename the type or give it ROCKS_CLASS(Name=\"...\")/ROCKS_STRUCT(Name=\"...\") with ASCII identifier segments joined by dots.").At(model.Site);
            }

            if (names.TryGetValue(toolName, out ClassModel? other))
            {
                string where = other.Imported ? $" of module {other.Module}" : "";
                throw new MetadataException($"{model.Name}: type tool name '{toolName}' collides with {other.Name}{where} (case-insensitive)", "RMC2203", hint).At(model.Site);
            }

            names.Add(toolName, model);
        }
    }
}
