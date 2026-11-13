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

    // descriptor import가 받는 값 종류다(MetaCompiler가 만들 수 있는 종류). Array는 B5에서 더한다.
    public static bool IsSupportedValueKind(string kind) => Scalars.ContainsValue(kind) || FieldModel.KindHasTargetType(kind);

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

        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            foreach (FieldModel field in external.Fields.Where(field => field.HasTargetType && field.ReferenceId is not null))
            {
                bool found = byId.TryGetValue(field.ReferenceId!, out ClassModel? target);
                if (!FieldModel.KindRequiresTargetId(field.Kind))
                {
                    // 강한·소프트 참조의 대상은 클래스다(import에 없는 대상은 런타임 Acquire가 판정한다).
                    if (found && (target!.IsEnum || target.IsStruct))
                    {
                        throw InvalidDescriptor(external, $"reference field {external.Name}::{field.Name} targets {(target.IsEnum ? "enum" : "struct")} {target.Name}");
                    }

                    continue;
                }

                string what = field.Kind.ToLowerInvariant();
                if (!found)
                {
                    throw new MetadataException($"{external.Name}::{field.Name}: {what} {field.ReferenceId} of the imported field is not in an imported descriptor", "RMC2003",
                        "Import the descriptors of every module the dependency uses; regenerate the dependency metadata.").At(external.Site);
                }

                if (target!.IsStruct != field.IsStruct || target.IsEnum != (field.Kind == "Enum"))
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
        foreach (ClassModel model in structs)
        {
            bool strong = model.Fields.Any(field => field.IsReference || (field.IsStruct && imported[field.Reference!].ContainsStrongReference));
            if (strong != model.ContainsStrongReference)
            {
                throw InvalidDescriptor(model, $"ContainsStrongReference flag of struct {model.Name} disagrees with its fields");
            }

            if (!model.RequiresWorldOnlyOwner && model.Fields.Any(field => field.IsStruct && imported[field.Reference!].RequiresWorldOnlyOwner))
            {
                throw InvalidDescriptor(model, $"struct {model.Name} holds a struct that requires a WorldOnly owner but lacks the RequiresWorldOnlyOwner flag");
            }
        }
    }

    // struct 필드 간선(값으로 담는 멤버 struct)을 DFS로 돌며 순환(RMC2302)을 찾고 깊이(1 + 멤버 struct 깊이의 최댓값,
    // 멤버 struct가 없으면 1)를 계산한다. 깊이가 StructRules.MaxDepth를 넘으면 RMC2303이다. 이미 깊이를 계산한
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
                    : "Break the cycle; a struct cannot contain itself by value directly or through nested struct members.").At(model.Site);
            }

            path.Add(model.Name);
            int depth = 1;
            foreach (FieldModel field in model.Fields.Where(field => field.IsStruct))
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

            IEnumerable<string> used = model.Fields.Where(field => field.IsStruct).Select(field => field.Reference!)
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

        // 리플렉션 enum(structs면 struct) 철자(desugar된 정규 이름)면 그 모델을 돌려준다(아니면 null). 다른 모듈의
        // private 타입은 RMC2006이다.
        ClassModel? ValueType(string spelling, bool structs, string owner, SourceSite site)
        {
            string name = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
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

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum))
        {
            if (!model.IsStruct)
            {
                model.Base = Resolve(model.Base, model.Name, model.Site);
            }

            foreach (FieldModel field in model.OwnFields)
            {
                if (Scalars.TryGetValue(field.NativeSpelling, out string? kind))
                {
                    field.Kind = kind;
                }
                else if (ValueType(field.NativeSpelling, false, field.Owner, field.Site) is ClassModel enumeration)
                {
                    field.Reference = enumeration.Name;
                    field.ReferenceId = enumeration.Id;
                    field.Kind = "Enum";
                }
                else if (ValueType(field.NativeSpelling, true, field.Owner, field.Site) is ClassModel structure)
                {
                    field.Reference = structure.Name;
                    field.ReferenceId = structure.Id;
                    field.Kind = "Struct";
                }
                else if (ReferenceSpelling(field.NativeSpelling) is { } reference)
                {
                    // 강한 참조와 소프트 참조(GC 간선이 아님) 모두 대상이 리플렉션 클래스(또는 Object)여야 한다.
                    field.Reference = Resolve(reference.Target.Length == 0 ? "rocks::Object" : reference.Target, model.Name, field.Site);
                    field.ReferenceId = field.Reference == "rocks::Object" ? null : Find(field.Reference).Id;
                    field.Kind = reference.Kind;
                }
                else
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: unsupported field type '{field.NativeSpelling}'",
                    hint: "Use a supported scalar, Guid, String, AssetId, Sha256Digest, ROCKS_ENUM enum, ROCKS_STRUCT struct, ObjectRef or SoftObjectRef field; raw pointers/containers/cv-qualified fields need an explicit codec extension.").At(field.Site);
                }

                // 지정자를 그 필드 종류에 쓸 수 있는지 본다(RMC2202). 배열 필드는 B5에서 생긴다.
                if (field.Spec.Required && !field.AcceptsRequired)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: Required applies only to object or soft object reference fields", "RMC2202",
                        "Remove Required or change the field to ObjectRef or SoftObjectRef.").At(field.Site);
                }

                if (field.Spec.MaxElements != 0)
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: MaxElements applies only to array fields", "RMC2202",
                        "Remove MaxElements; it limits the length of reflected array fields.").At(field.Site);
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

        // struct: 필드 목록 → 순환·깊이(DFS, Order·생성 전 — PROPERTY 멤버만) → 순위(PROPERTY·NATIVE struct 멤버) →
        // 형질(쓰는 struct가 먼저 계산되도록 순위 순).
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
            // Transient 참조도 GC가 추적하므로 강한 참조로 센다.
            model.ContainsStrongReference = model.Fields.Any(field => field.IsReference || (field.IsStruct && Find(field.Reference!).ContainsStrongReference));
            // NATIVE 멤버의 WorldOnly 소유자 형질은 struct에서는 오류가 아니라 형질로 전파된다(Docs/Design/
            // ReflectionFoundations.ko.md 2.4절).
            model.RequiresWorldOnlyOwner = model.WorldOnlyNatives.Count != 0 ||
                model.Fields.Any(field => field.IsStruct && Find(field.Reference!).RequiresWorldOnlyOwner) ||
                model.NativeMembers.Any(native => native.Struct is not null && Find(native.Struct).RequiresWorldOnlyOwner);
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

            // 그 형질을 가진 struct를 (상속 필드·NATIVE 멤버 포함) 담는 클래스도 WorldOnly여야 한다(RMC2206).
            FieldModel? worldOnlyField = model.WorldOnly ? null
                : model.Fields.FirstOrDefault(field => field.IsStruct && Find(field.Reference!).RequiresWorldOnlyOwner);
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
