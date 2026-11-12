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
        ["rocks::String"] = "Utf8String"
    };

    // base·참조 대상은 이 모듈 타입 → import 타입 순으로 정규 이름을 찾는다. 다른 모듈의 private 타입은
    // 쓸 수 없다(RMC2006).
    public static SchemaModel Validate(Dictionary<string, ClassModel> classes, IReadOnlyDictionary<string, ClassModel> imported)
    {
        string Resolve(string spelling, string owner, SourceSite site)
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
        foreach (ClassModel model in classes.Values)
        {
            model.Base = Resolve(model.Base, model.Name, model.Site);
            foreach (FieldModel field in model.OwnFields)
            {
                if (Scalars.TryGetValue(field.NativeSpelling, out string? kind))
                {
                    field.Kind = kind;
                }
                else if (field.NativeSpelling.StartsWith("rocks::ObjectRef<", StringComparison.Ordinal) && field.NativeSpelling.EndsWith('>'))
                {
                    string target = field.NativeSpelling[17..^1].Trim();
                    field.Reference = Resolve(target.Length == 0 ? "rocks::Object" : target, model.Name, field.Site);
                    field.ReferenceId = field.Reference == "rocks::Object" ? null : Find(field.Reference).Id;
                    field.Kind = "ObjectReference";
                }
                else
                {
                    throw new MetadataException($"{field.Owner}::{field.Name}: unsupported field type '{field.NativeSpelling}'",
                    hint: "Use a supported scalar, Guid, String or ObjectRef field; raw pointers/containers/cv-qualified fields need an explicit codec extension.").At(field.Site);
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
                if (classes.TryGetValue(model.Base, out ClassModel? ownBase))
                {
                    Order(ownBase);
                    inherited.AddRange(ownBase.Fields);
                }
                else
                {
                    inherited.AddRange(imported[model.Base].Fields); // import 타입의 Fields는 상속 포함 전체다
                }
            }

            model.Fields = inherited.Concat(model.OwnFields).OrderBy(field => field.Id, StringComparer.Ordinal).ToList();
            if (model.Fields.Count > 4096)
            {
                throw new MetadataException($"{model.Name}: inherited field count exceeds 4096").At(model.Site);
            }

            visiting.Remove(model.Name);
            visited.Add(model.Name);
            ordered.Add(model);
        }

        foreach (ClassModel model in classes.Values.OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            Order(model);
        }

        return new(classes, ordered, imported);
    }
}
