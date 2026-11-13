namespace Rocks.MetaCompiler;

internal static class SchemaValidator
{
    private static readonly Dictionary<string, string> Scalars = new(StringComparer.Ordinal)
    {
        ["bool"] = "Bool",
        ["unsigned char"] = "UInt8",
        ["unsigned short"] = "UInt16",
        ["unsigned int"] = "UInt32",
        ["unsigned long long"] = "UInt64",
        ["int"] = "Int32",
        ["long long"] = "Int64",
        ["float"] = "Float32",
        ["double"] = "Float64",
        ["rocks::Guid"] = "Guid",
        ["rocks::String"] = "Utf8String",
        ["rocks::AssetId"] = "AssetId",
        ["rocks::Sha256Digest"] = "Digest256"
    };

    // descriptor import가 받는 값 종류다(MetaCompiler가 만들 수 있는 종류 — 배열 필드의 Array 포함).
    public static bool IsSupportedValueKind(string kind) => IsSupportedElementKind(kind) || kind == ArrayRules.Array;

    // 배열 원소로 받는 값 종류다(Array 자신은 원소가 될 수 없다 — 중첩 배열은 RMC2304).
    public static bool IsSupportedElementKind(string kind) => Scalars.ContainsValue(kind) || FieldModel.KindHasTargetType(kind);

    // "prefix<T>" 철자의 T를 돌려준다(아니면 null).
    private static string? TemplateTarget(string spelling, string prefix) =>
        spelling.StartsWith(prefix, StringComparison.Ordinal) && spelling.EndsWith('>') ? spelling[prefix.Length..^1].Trim() : null;

    // 강한 참조(ObjectRef<T>)·소프트 참조(SoftObjectRef<T>) 철자면 대상 T와 값 종류를 돌려준다(아니면 null).
    private static (string Target, string Kind)? ReferenceSpelling(string spelling)
    {
        if (TemplateTarget(spelling, "rocks::ObjectRef<") is string strong)
        {
            return (strong, "ObjectReference");
        }

        if (TemplateTarget(spelling, "rocks::SoftObjectRef<") is string soft)
        {
            return (soft, "SoftObjectReference");
        }

        return null;
    }

    private static MetadataException InvalidDescriptor(ClassModel model, string reason) => new(
        $"invalid descriptor {model.Site.File}: {reason}", "RMC1000",
        "Regenerate the dependency module's metadata; descriptors are build outputs and must not be edited.");

    // import한 타입을 서로 잇는다(빌드가 넘긴 닫힌 descriptor 집합 — CompilerDriver.LoadImports가 부른다). import
    // 필드의 Enum·Struct 대상은 descriptor에 ID만 있으므로 그 ID의 import 타입으로 Reference를 채운다(이 모듈이 상속·
    // 포함하면 생성 헤더가 그 타입의 생성 헤더를 include하고 native 키를 싣는다). 대상 ID가 다른 종류의 타입이면
    // (Struct 필드가 enum을, 참조가 enum·struct를 가리키면) descriptor가 망가진 것이므로 RMC1000, 대상이 import에
    // 없으면 RMC2003, 다른 모듈의 private 타입이거나 public 타입이 private 타입을 쓰면 RMC2006이다. 이어서 import
    // struct 사이 순환(RMC2302)과 깊이(RMC2303 — descriptor에 깊이 키는 없다)를 이 모듈 struct와 같은 DFS로 검사하고,
    // struct flags가 필드·멤버 struct와 맞는지 대조한다(어긋나면 RMC1000).
    public static void LinkImported(IReadOnlyDictionary<string, ClassModel> imported)
    {
        Dictionary<string, ClassModel> byId = new(StringComparer.Ordinal);
        foreach (ClassModel external in imported.Values)
        {
            byId.TryAdd(external.Id, external);
        }

        // Binds 미러는 CoreTypeBindings만 내고, 한 실제 타입은 미러 하나만 묶으며, 리플렉션 타입이나 미러 자신을 묶을 수
        // 없다(같은 C++ 타입에 GeneratedType 특수화가 둘이 되거나 리플렉션 타입과 겹친다).
        Dictionary<string, ClassModel> bound = new(StringComparer.Ordinal);
        foreach (ClassModel mirror in imported.Values.Where(model => model.Binds.Length != 0).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            string? conflict = !string.Equals(mirror.Module, StructRules.BindingModule, StringComparison.Ordinal)
                ? $"module {mirror.Module} declares Binds; only {StructRules.BindingModule} binds Core value types"
                : bound.TryGetValue(mirror.Binds, out ClassModel? other) ? $"{mirror.Name} and {other.Name} both bind {mirror.Binds}"
                : imported.ContainsKey(mirror.Binds) ? $"{mirror.Name} binds reflected type {mirror.Binds}" : null;
            if (conflict is not null)
            {
                throw new MetadataException($"imported Binds mirror {mirror.Name}: {conflict}", "RMC2004",
                    "Regenerate the dependency metadata; each Core value type is bound once, by CoreTypeBindings.").At(mirror.Site);
            }

            bound.Add(mirror.Binds, mirror);
        }

        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            // 대상은 배열이면 원소의 대상이다(원소 enum·struct·참조 대상도 같은 규칙으로 잇고 대조한다).
            foreach (FieldModel field in external.Fields.Where(field => field.HasTargetType && field.ReferenceId is not null))
            {
                bool found = byId.TryGetValue(field.ReferenceId!, out ClassModel? target);
                if (!FieldModel.KindRequiresTargetId(field.TargetKind))
                {
                    // 강한·소프트 참조의 대상은 클래스다(import에 없는 대상은 런타임 Acquire가 판정한다).
                    if (found && (target!.IsEnum || target.IsStruct))
                    {
                        throw InvalidDescriptor(external, $"reference field {external.Name}::{field.Name} targets {(target.IsEnum ? "enum" : "struct")} {target.Name}");
                    }

                    continue;
                }

                string what = field.TargetKind.ToLowerInvariant();
                if (!found)
                {
                    throw new MetadataException($"{external.Name}::{field.Name}: {what} {field.ReferenceId} of the imported field is not in an imported descriptor", "RMC2003",
                        "Import the descriptors of every module the dependency uses; regenerate the dependency metadata.").At(external.Site);
                }

                if (target!.IsStruct != field.StructTarget || target.IsEnum != (field.TargetKind == "Enum"))
                {
                    throw InvalidDescriptor(external, $"{what} field {external.Name}::{field.Name} targets {target.Name}, which is not a reflected {what}");
                }

                if (target.Private && (!external.Private || !string.Equals(target.Module, external.Module, StringComparison.OrdinalIgnoreCase)))
                {
                    throw new MetadataException($"{external.Name}::{field.Name}: imported type of module {external.Module} uses private {what} {target.Name} of module {target.Module}", "RMC2006",
                        "Regenerate the dependency metadata; only private types of the same module can use private enums or structs.").At(external.Site);
                }

                field.Reference = target.Name;
            }
        }

        ClassModel[] structs = imported.Values.Where(model => model.IsStruct).ToArray();
        ComputeDepths(structs, name => imported[name]);
        ClassModel Find(string name) => imported[name];
        foreach (ClassModel model in structs)
        {
            bool strong = model.Fields.Any(field => field.IsObjectReferenceFieldKind || (field.StructTarget && Find(field.Reference!).ContainsStrongReference));
            if (strong != model.ContainsStrongReference)
            {
                throw InvalidDescriptor(model, $"ContainsStrongReference flag of struct {model.Name} disagrees with its fields");
            }

            if (!model.RequiresWorldOnlyOwner && model.Fields.Any(field => field.StructTarget && Find(field.Reference!).RequiresWorldOnlyOwner))
            {
                throw InvalidDescriptor(model, $"struct {model.Name} holds a struct that requires a WorldOnly owner but lacks the RequiresWorldOnlyOwner flag");
            }

            // 형질은 descriptor flags로만 비교한다(멤버 struct의 형질도 그 항목의 flags) — import 순회 순서에 의존하지 않는다.
            if (ContainsReferenceArray(model, Find) != model.ContainsReferenceArray)
            {
                throw InvalidDescriptor(model, $"ContainsReferenceArray flag of struct {model.Name} disagrees with its fields");
            }
        }

        // StructRefArray의 원소 struct(배열 필드의 원소 struct가 참조 struct)는 Registry 컨테이너를 담지 않는다(원소 블록이
        // owner 저장소 밖이라 Registry가 그 컨테이너의 변경을 거부한다 — 이 모듈 선언이면 RMC2304).
        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            FieldModel? nested = external.Fields.FirstOrDefault(field => field.IsArray && field.StructTarget &&
                Find(field.Reference!).ContainsStrongReference && Find(field.Reference!).ContainsReferenceArray);
            if (nested is not null)
            {
                throw InvalidDescriptor(external, $"StructRefArray field {external.Name}::{nested.Name} has element struct {nested.Reference} that holds a reference array");
            }
        }
    }

    // struct의 ContainsReferenceArray 형질을 필드로 계산한다: Registry 컨테이너(ObjectRefArray·StructRefArray) 필드이거나
    // 그 형질을 가진 struct 멤버(런타임 RegisterStruct와 같은 규칙).
    private static bool ContainsReferenceArray(ClassModel model, Func<string, ClassModel> find) =>
        model.Fields.Any(field => field.IsRegistryContainer(find) || (field.IsStruct && find(field.Reference!).ContainsReferenceArray));

    // struct 필드 간선(값으로 담는 멤버 struct와 struct 원소 배열의 원소 struct)을 DFS로 돌며 순환(RMC2302 — 배열을 거친
    // 자기 참조 StructRefArray<Self> 포함)을 찾고 깊이(1 + 멤버·원소 struct 깊이의 최댓값, 없으면 1)를 계산한다. 깊이가
    // StructRules.MaxDepth를 넘으면 RMC2303이다. 이미 깊이를 계산한
    // import struct는 다시 돌지 않는다. Order·깊이·생성보다 먼저 실행한다(순환이 있으면 재귀가 끝나지 않는다).
    // Binds(B4c)가 생기면 C++에서도 순환이 생길 수 있으므로 간선은 정규 이름으로 푼 것이다. import struct의 위반은
    // 의존 모듈 descriptor의 문제이므로 그 메타데이터를 다시 만들라고 안내한다.
    private static void ComputeDepths(IEnumerable<ClassModel> structs, Func<string, ClassModel> find)
    {
        const string regenerate = "Regenerate the dependency module's metadata; its descriptor must come from a valid MetaCompiler run.";
        HashSet<string> done = new(StringComparer.Ordinal);
        List<string> path = [];

        int Depth(ClassModel model)
        {
            if (done.Contains(model.Name) || (model.Imported && model.Depth > 0))
            {
                return model.Depth;
            }

            if (path.Contains(model.Name, StringComparer.Ordinal))
            {
                string cycle = string.Join(" -> ", path.SkipWhile(name => name != model.Name).Append(model.Name));
                throw new MetadataException($"{model.Name}: reflected struct type cycle ({cycle})", "RMC2302", model.Imported ? regenerate
                    : "Break the cycle; a struct cannot contain itself by value directly or through nested struct members or struct element arrays.").At(model.Site);
            }

            path.Add(model.Name);
            int depth = 1;
            // struct 멤버와 struct 원소 배열 멤버(원소 깊이 + 1)가 간선이다.
            foreach (FieldModel field in model.Fields.Where(field => field.StructTarget))
            {
                depth = Math.Max(depth, 1 + Depth(find(field.Reference!)));
            }

            path.RemoveAt(path.Count - 1);
            if (depth > StructRules.MaxDepth)
            {
                throw new MetadataException($"{model.Name}: reflected struct nesting depth {depth} exceeds {StructRules.MaxDepth}", "RMC2303", model.Imported ? regenerate
                    : $"Flatten the nested struct members; reflected structs nest at most {StructRules.MaxDepth} levels (MaxStructDepth).").At(model.Site);
            }

            model.Depth = depth;
            done.Add(model.Name);
            return depth;
        }

        foreach (ClassModel model in structs.OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Depth(model);
        }
    }

    // 이 모듈 struct의 생성·등록 순위를 계산한다: 1 + (이 모듈 struct인 PROPERTY struct 멤버와 NATIVE 리플렉션 struct
    // 멤버의 순위) 최댓값. 생성 특수화는 NATIVE 멤버 struct의 GeneratedType도 static_assert로 쓰므로 깊이(PROPERTY만,
    // RMC2303)가 아니라 이 순위로 정렬해야 쓰는 특수화가 먼저 나온다. import struct는 다른 생성 헤더에 있으므로 세지
    // 않는다. 값 멤버로는 순환이 생길 수 없지만(불완전 타입) 순환이면 RMC2302다.
    private static void ComputeRanks(IEnumerable<ClassModel> structs, IReadOnlyDictionary<string, ClassModel> classes)
    {
        HashSet<string> done = new(StringComparer.Ordinal);
        HashSet<string> visiting = new(StringComparer.Ordinal);

        int Rank(ClassModel model)
        {
            if (done.Contains(model.Name))
            {
                return model.Rank;
            }

            if (!visiting.Add(model.Name))
            {
                throw new MetadataException($"{model.Name}: reflected struct type cycle through NATIVE members", "RMC2302",
                    "Break the cycle; a struct cannot contain itself by value directly or through nested struct members.").At(model.Site);
            }

            IEnumerable<string> used = model.Fields.Where(field => field.StructTarget).Select(field => field.Reference!)
                .Concat(model.NativeMembers.Where(native => native.Struct is not null).Select(native => native.Struct!));
            int rank = 1;
            foreach (string name in used)
            {
                if (classes.TryGetValue(name, out ClassModel? own) && own.IsStruct)
                {
                    rank = Math.Max(rank, 1 + Rank(own));
                }
            }

            visiting.Remove(model.Name);
            model.Rank = rank;
            done.Add(model.Name);
            return rank;
        }

        foreach (ClassModel model in structs.OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Rank(model);
        }
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
            if (structs && ((classes.TryGetValue(name, out ClassModel? named) || imported.TryGetValue(name, out named)) && named.Binds.Length != 0))
            {
                throw new MetadataException($"{owner}: '{name}' is a Binds mirror declaration; use the real type {named.Binds}", "RMC2000",
                    $"Declare the field with the real type ({named.Binds}); the mirror only describes it to the metadata compiler.").At(site);
            }

            if (structs && !classes.ContainsKey(name) && !imported.ContainsKey(name) && mirrors.TryGetValue(name, out ClassModel? mirror))
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

            return "Use a supported scalar, Guid, String, AssetId, Sha256Digest, ROCKS_ENUM enum, ROCKS_STRUCT struct, ObjectRef, SoftObjectRef or rocks::Array/ObjectRefArray/StructRefArray field; raw pointers/other containers/cv-qualified fields need an explicit codec extension.";
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
                // 강한 참조와 소프트 참조(GC 간선이 아님) 모두 대상이 리플렉션 클래스(또는 Object)여야 한다.
                string target = Resolve(reference.Target.Length == 0 ? "rocks::Object" : reference.Target, owner, site);
                return (reference.Kind, target, target == "rocks::Object" ? null : Find(target).Id);
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

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum))
        {
            if (!model.IsStruct)
            {
                model.Base = Resolve(model.Base, model.Name, model.Site);
            }

            foreach (FieldModel field in model.OwnFields)
            {
                if (ArrayRules.Split(field.NativeSpelling) is (string container, string element))
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

                // 지정자를 그 필드 종류에 쓸 수 있는지 본다(RMC2202 — 종류를 먼저 판정하고 값 범위는 그 뒤에 본다).
                if (field.Spec.Required && !field.AcceptsRequired)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: Required applies only to object or soft object reference fields and arrays of them", "RMC2202",
                        "Remove Required or change the field to ObjectRef, SoftObjectRef, ObjectRefArray or Array<SoftObjectRef>.").At(field.Site);
                }

                if (!field.IsArray && field.Spec.MaxElements != 0)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: MaxElements applies only to array fields", "RMC2202",
                        "Remove MaxElements; it limits the length of reflected array fields.").At(field.Site);
                }

                if (field.IsArray)
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
                native.Struct = ValueType(ClangAstParser.CleanType(native.Spelling), true, model.Name, native.Site)?.Name;
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
                throw new MetadataException($"{symbol}: {field.SpelledContainer} element struct {field.Reference} holds ObjectRefArray or StructRefArray members", "RMC2304",
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

        // struct: 필드 목록 → 순환·깊이(DFS, Order·생성 전 — PROPERTY struct 멤버·struct 원소 배열 멤버) → 순위(PROPERTY·
        // NATIVE struct 멤버와 원소 struct) → 형질(쓰는 struct가 먼저 계산되도록 순위 순)·컨테이너 대조.
        ClassModel[] structs = classes.Values.Where(model => model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal).ToArray();
        foreach (ClassModel model in structs)
        {
            model.Fields = BuildFields(model, []);
        }

        ComputeDepths(structs, Find);
        ComputeRanks(structs, classes);
        ClassModel[] orderedStructs = structs.OrderBy(model => model.Rank).ThenBy(model => model.Name, StringComparer.Ordinal).ToArray();
        foreach (ClassModel model in orderedStructs)
        {
            // Transient 참조도 GC가 추적하므로 강한 참조로 센다. ObjectRefArray 필드와 참조 struct 원소 배열(StructRefArray)도
            // 강한 참조다.
            model.ContainsStrongReference = model.Fields.Any(field => field.IsObjectReferenceFieldKind || (field.StructTarget && Find(field.Reference!).ContainsStrongReference));
            // NATIVE 멤버의 WorldOnly 소유자 형질은 struct에서는 오류가 아니라 형질로 전파된다(Docs/Design/
            // ReflectionFoundations.ko.md 2.4절). 원소 struct의 형질도 전파된다.
            model.RequiresWorldOnlyOwner = model.WorldOnlyNatives.Count != 0 ||
                model.Fields.Any(field => field.StructTarget && Find(field.Reference!).RequiresWorldOnlyOwner) ||
                model.NativeMembers.Any(native => native.Struct is not null && Find(native.Struct).RequiresWorldOnlyOwner);
            model.ContainsReferenceArray = ContainsReferenceArray(model, Find);
            foreach (FieldModel field in model.OwnFields)
            {
                CheckContainer(field);
            }
        }

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum && !model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            foreach (FieldModel field in model.OwnFields)
            {
                CheckContainer(field);
            }
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

        // enum은 그 enum을 쓰는 타입보다, struct는 그 struct를 쓰는 struct·클래스보다 먼저 등록해야 하므로 앞에 둔다.
        // struct는 깊이 순(멤버 struct가 먼저)이고 같은 깊이는 이름 순이다.
        ordered.AddRange(classes.Values.Where(model => model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal));
        ordered.AddRange(orderedStructs);
        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum && !model.IsStruct).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Order(model);
        }

        return new(classes, ordered, imported);
    }
}
