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

    // descriptor import가 받는 값 종류다(MetaCompiler가 만들 수 있는 종류). Struct·Array는 B4·B5에서 더한다.
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

    // base·참조 대상은 이 모듈 타입 → import 타입 순으로 정규 이름을 찾는다. 다른 모듈의 private 타입은
    // 쓸 수 없다(RMC2006). base·참조 대상은 클래스여야 하고 Enum 필드의 타입은 리플렉션 enum이어야 한다.
    public static SchemaModel Validate(Dictionary<string, ClassModel> classes, IReadOnlyDictionary<string, ClassModel> imported)
    {
        string Resolve(string spelling, string owner, SourceSite site)
        {
            string resolved = ResolveType(spelling, owner, site);
            if (resolved != "rocks::Object" && Find(resolved).IsEnum)
            {
                throw new MetadataException($"{owner}: base/reference type '{resolved}' is a reflected enum, not a class", "RMC2003",
                    "Use a reflected class as the base or reference target; enums are values.").At(site);
            }

            return resolved;
        }

        // 리플렉션 enum 철자(desugar된 정규 이름)면 그 모델을 돌려준다(아니면 null). 다른 모듈의 private enum은 RMC2006이다.
        ClassModel? EnumType(string spelling, string owner, SourceSite site)
        {
            string name = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
            if (classes.TryGetValue(name, out ClassModel? own))
            {
                return own.IsEnum ? own : null;
            }

            if (!imported.TryGetValue(name, out ClassModel? external) || !external.IsEnum)
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

        // import 클래스의 Enum 필드는 descriptor에 enum ID만 있다. 그 ID의 import enum으로 Reference를 채운다(이 모듈이
        // 상속하면 생성 헤더가 그 enum의 생성 헤더를 include하고 native 키를 싣는다). 의존 descriptor는 전이 의존까지
        // import되므로 찾지 못하면 descriptor가 어긋난 것이다.
        Dictionary<string, ClassModel> importedEnums = new(StringComparer.Ordinal);
        foreach (ClassModel external in imported.Values.Where(model => model.IsEnum))
        {
            importedEnums.TryAdd(external.Id, external);
        }

        foreach (ClassModel external in imported.Values.Where(model => !model.IsEnum))
        {
            foreach (FieldModel field in external.Fields.Where(field => field.Kind == "Enum"))
            {
                if (field.ReferenceId is null || !importedEnums.TryGetValue(field.ReferenceId, out ClassModel? target))
                {
                    throw new MetadataException($"{external.Name}::{field.Name}: enum {field.ReferenceId} of the imported field is not in an imported descriptor", "RMC2003",
                        "Import the descriptors of every module the dependency uses; regenerate the dependency metadata.").At(external.Site);
                }

                if (target.Private && !external.Private)
                {
                    throw new MetadataException($"{external.Name}::{field.Name}: public imported type uses private enum {target.Name}", "RMC2006",
                        "Regenerate the dependency metadata; public types cannot use private enums.").At(external.Site);
                }

                field.Reference = target.Name;
            }
        }

        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum))
        {
            model.Base = Resolve(model.Base, model.Name, model.Site);
            foreach (FieldModel field in model.OwnFields)
            {
                if (Scalars.TryGetValue(field.NativeSpelling, out string? kind))
                {
                    field.Kind = kind;
                }
                else if (EnumType(field.NativeSpelling, field.Owner, field.Site) is ClassModel enumeration)
                {
                    field.Reference = enumeration.Name;
                    field.ReferenceId = enumeration.Id;
                    field.Kind = "Enum";
                }
                else if (ReferenceSpelling(field.NativeSpelling) is { } reference)
                {
                    // 강한 참조와 소프트 참조(GC 간선이 아님) 모두 대상이 리플렉션 타입(또는 Object)이어야 한다.
                    field.Reference = Resolve(reference.Target.Length == 0 ? "rocks::Object" : reference.Target, model.Name, field.Site);
                    field.ReferenceId = field.Reference == "rocks::Object" ? null : Find(field.Reference).Id;
                    field.Kind = reference.Kind;
                }
                else
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: unsupported field type '{field.NativeSpelling}'",
                    hint: "Use a supported scalar, Guid, String, AssetId, Sha256Digest, ROCKS_ENUM enum, ObjectRef or SoftObjectRef field; raw pointers/containers/cv-qualified fields need an explicit codec extension.").At(field.Site);
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

            model.Fields = inherited.Concat(model.OwnFields).OrderBy(field => field.Id, StringComparer.Ordinal).ToList();
            if (model.Fields.Count > 4096)
            {
                throw new MetadataException($"{model.Name}: inherited field count exceeds 4096").At(model.Site);
            }

            // 도구 이름은 상속 포함 대소문자 무시로 유일하다(RMC2203).
            Dictionary<string, FieldModel> toolNames = new(StringComparer.OrdinalIgnoreCase);
            foreach (FieldModel field in model.Fields)
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

            // WorldOnly 소유자 형질을 담는 NATIVE 멤버는 WorldOnly 클래스(상속 포함)에만 둔다(RMC2206).
            foreach ((string symbol, string path, SourceSite site) in model.WorldOnlyNatives)
            {
                if (!model.WorldOnly)
                {
                    throw new MetadataException($"{symbol}: NATIVE member requires a WorldOnly owner ({path})", "RMC2206",
                        "Mark the class ROCKS_CLASS(WorldOnly) or move the member to a World-side type.").At(site);
                }
            }

            visiting.Remove(model.Name);
            visited.Add(model.Name);
            ordered.Add(model);
        }

        // enum은 그 enum을 쓰는 클래스보다 먼저 등록해야 하므로 앞에 둔다(필드가 없고 base도 없다).
        ordered.AddRange(classes.Values.Where(model => model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal));
        foreach (ClassModel model in classes.Values.Where(model => !model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Order(model);
        }

        return new(classes, ordered, imported);
    }
}
