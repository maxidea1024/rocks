namespace Rocks.MetaCompiler;

// SchemaValidator 중 import descriptor 잇기(LinkImported — 대상 ID 잇기·private 규칙·struct flags 대조)와 struct 그래프
// 도우미(ContainsReferenceArray 형질, ComputeDepths 순환·깊이 DFS, ComputeRanks 생성 순위)다. 원본 선언의 철자 해석과 검증
// (Validate)은 SchemaValidator.cs에 있다.
internal static partial class SchemaValidator
{
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

        // import 필드의 대상 하나(kind·referenceId — 값·원소·내부·Map 값의 대상이면 TargetKind, Map 키면 KeyKind)를 import 타입으로
        // 잇고 정규 이름을 돌려준다. 강한·weak·소프트 참조의 기대 타입과 ClassRef의 Base는 클래스다(import에 없는 대상은 런타임
        // Acquire가 판정한다 — 생성 코드는 그 클래스의 생성 헤더를 쓰지 않는다 — null). Enum·Struct 대상은 import에 있어야 하고
        // (RMC2003) 그 종류여야 하며(RMC1000) 다른 모듈의 private 타입이면 RMC2006이다. key는 Map 키 대상인지다(진단 문구만 다르다).
        string? LinkTarget(ClassModel external, FieldModel field, string kind, string referenceId, bool key)
        {
            bool found = byId.TryGetValue(referenceId, out ClassModel? target);
            string symbol = $"{external.Name}::{field.Name}";
            // InstancedStruct<Base>(리플렉션 B9k)의 Base는 struct 대상과 같이 잇는다.
            bool instanced = kind == "InstancedStruct";
            if (kind is not ("Enum" or "Struct") && !instanced)
            {
                if (found && (target!.IsEnum || target.IsStruct))
                {
                    throw InvalidDescriptor(external, $"{(key ? "map key of" : "reference field")} {symbol} targets {(target.IsEnum ? "enum" : "struct")} {target.Name}");
                }

                return null;
            }

            string what = instanced ? "struct" : kind.ToLowerInvariant();
            if (!found)
            {
                throw new MetadataException($"{symbol}: {(key ? "key " : "")}{what} {referenceId} of the imported field is not in an imported descriptor", "RMC2003",
                    "Import the descriptors of every module the dependency uses; regenerate the dependency metadata.").At(external.Site);
            }

            if (target!.IsStruct != (kind == "Struct" || instanced) || target.IsEnum != (kind == "Enum"))
            {
                throw InvalidDescriptor(external, $"{(key ? "enum key of map field" : what + " field")} {symbol} targets {target.Name}, which is not a reflected {what}");
            }

            if (target.Private && (!external.Private || !string.Equals(target.Module, external.Module, StringComparison.OrdinalIgnoreCase)))
            {
                throw new MetadataException($"{symbol}: imported type of module {external.Module} uses private {what} {target.Name} of module {target.Module}", "RMC2006",
                    "Regenerate the dependency metadata; only private types of the same module can use private enums or structs.").At(external.Site);
            }

            return target.Name;
        }

        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            // 대상은 배열이면 원소의 대상이다(원소 enum·struct·참조 대상도 같은 규칙으로 잇고 대조한다).
            foreach (FieldModel field in external.Fields.Where(field => field.HasTargetType && field.ReferenceId is not null))
            {
                if (LinkTarget(external, field, field.TargetKind, field.ReferenceId!, false) is string reference)
                {
                    field.Reference = reference;
                }
            }

            // Map 키의 대상(리플렉션 B9f2): Enum 키는 그 import enum으로 잇고(생성 헤더 include·native 키), ClassRef·Soft 키의 대상은 클래스다.
            foreach (FieldModel field in external.Fields.Where(field => field.IsMap && field.KeyHasTargetType && field.KeyReferenceId is not null))
            {
                if (LinkTarget(external, field, field.KeyKind!, field.KeyReferenceId!, true) is string reference)
                {
                    field.KeyReference = reference;
                }
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

            // Optional<S>의 S와 Map 값 struct는 값 struct다(원본 선언이면 RMC2306·RMC2305).
            if (model.Fields.Any(field => (field.IsOptional || field.IsMap) && field.StructTarget && Find(field.Reference!).ContainsStrongReference))
            {
                throw InvalidDescriptor(model, $"struct {model.Name} holds an Optional or map of a reference struct");
            }

            // 형질은 descriptor flags로만 비교한다(멤버 struct의 형질도 그 항목의 flags) — import 순회 순서에 의존하지 않는다.
            if (ContainsReferenceArray(model, Find) != model.ContainsReferenceArray)
            {
                throw InvalidDescriptor(model, $"ContainsReferenceArray flag of struct {model.Name} disagrees with its fields");
            }
        }

        // StructRefArray의 원소 struct(배열 필드의 원소 struct가 참조 struct)는 Registry 컨테이너를 담지 않는다(원소 블록이
        // owner 저장소 밖이라 Registry가 그 컨테이너의 변경을 거부한다 — 이 모듈 선언이면 RMC2304). 클래스의 Optional<S>도 값 struct만
        // 담는다(원본 선언이면 RMC2306).
        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            if (!external.IsStruct && external.Fields.Any(field => (field.IsOptional || field.IsMap) && field.StructTarget && Find(field.Reference!).ContainsStrongReference))
            {
                throw InvalidDescriptor(external, $"class {external.Name} holds an Optional or map of a reference struct");
            }

            FieldModel? nested = external.Fields.FirstOrDefault(field => field.IsArray && field.StructTarget &&
                Find(field.Reference!).ContainsStrongReference && Find(field.Reference!).ContainsReferenceArray);
            if (nested is not null)
            {
                throw InvalidDescriptor(external, $"StructRefArray field {external.Name}::{nested.Name} has element struct {nested.Reference} that holds a reference array");
            }

            // 정적 배열의 원소 struct도 Registry 컨테이너를 담지 않는다(원본 선언이면 RMC2307 — 리플렉션 B9g).
            FieldModel? fixedNested = external.Fields.FirstOrDefault(field => field.IsStaticArray && field.StructTarget && Find(field.Reference!).ContainsReferenceArray);
            if (fixedNested is not null)
            {
                throw InvalidDescriptor(external, $"static array field {external.Name}::{fixedNested.Name} has element struct {fixedNested.Reference} that holds a reference array");
            }
        }
    }

    // struct의 ContainsReferenceArray 형질을 필드로 계산한다: Registry 컨테이너(ObjectRefArray·StructRefArray·ObjectRefMap) 필드이거나
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
            // struct 멤버, struct 원소 배열 멤버(원소 깊이 + 1), Optional<struct> 멤버(내부 깊이 + 1 — 리플렉션 B9e), Map 값 struct
            // 멤버(값 깊이 + 1 — 리플렉션 B9f2)가 간선이다.
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
    // struct가 InstancedStruct 값을 (중첩 struct까지) 담는지다(리플렉션 B9j·B9k — 런타임 형질 ContainsInstancedStruct와 같은 규칙).
    internal static bool ContainsInstancedStructTrait(ClassModel type, Func<string, ClassModel> find) =>
        type.Fields.Any(field => field.TargetKind == "InstancedStruct" || (field.StructTarget && ContainsInstancedStructTrait(find(field.Reference!), find)));

    // struct가 Required 멤버를 (중첩 struct까지, Transient 아래 빼고) 담는지다(런타임 형질 ContainsRequired와 같은 규칙).
    internal static bool ContainsRequiredTrait(ClassModel type, Func<string, ClassModel> find) =>
        type.Fields.Any(field => !field.Spec.Transient && (field.Spec.Required || (field.StructTarget && ContainsRequiredTrait(find(field.Reference!), find))));

    // struct가 (멤버 struct·base struct까지) AssetBundles 멤버를 담는가다(EditorOnly 필드 안 목록 거부 — Docs/Design/AssetSystem 10.2절 A4e2a).
    // 이 모듈 타입은 검증 중이라 Fields가 아직 비었을 수 있어 OwnFields(+ base)를, import 타입은 descriptor의 Fields(상속 포함)를 본다.
    internal static bool ContainsAssetBundles(ClassModel type, Func<string, ClassModel> find) =>
        (type.Imported ? type.Fields : type.OwnFields).Any(field => field.Spec.AssetBundles.Length != 0 || (field.StructTarget && ContainsAssetBundles(find(field.Reference!), find))) ||
        (type.IsStruct && type.Base.Length != 0 && ContainsAssetBundles(find(type.Base), find));

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

            // base struct와 InstancedStruct<Base>의 Base도 먼저 등록해야 한다(리플렉션 B9k).
            IEnumerable<string> used = model.Fields.Where(field => field.StructTarget).Select(field => field.Reference!)
                .Concat(model.NativeMembers.Where(native => native.Struct is not null).Select(native => native.Struct!))
                .Concat(model.Fields.Where(field => field.TargetKind == "InstancedStruct" && field.Reference is not null).Select(field => field.Reference!))
                .Concat(model.Base.Length != 0 ? [model.Base] : []);
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
}
