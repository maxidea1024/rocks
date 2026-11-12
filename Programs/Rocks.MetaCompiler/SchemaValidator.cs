namespace Rocks.MetaCompiler;

internal static class SchemaValidator
{
    private static readonly Dictionary<string, string> Scalars = new(StringComparer.Ordinal)
    {
        ["bool"] = "Bool", ["unsigned char"] = "UInt8", ["unsigned short"] = "UInt16",
        ["unsigned int"] = "UInt32", ["unsigned long long"] = "UInt64", ["int"] = "Int32",
        ["long long"] = "Int64", ["float"] = "Float32", ["double"] = "Float64",
        ["rocks::Guid"] = "Guid", ["rocks::String"] = "Utf8String"
    };

    public static SchemaModel Validate(Dictionary<string, ClassModel> classes)
    {
        string Resolve(string spelling, string owner, SourceSite site)
        {
            spelling = spelling.StartsWith("::", StringComparison.Ordinal) ? spelling[2..] : spelling;
            if (spelling == "rocks::Object" || classes.ContainsKey(spelling)) return spelling;
            string[] scope = owner.Split("::");
            for (int count = scope.Length - 1; count > 0; --count)
            {
                string candidate = string.Join("::", scope.Take(count)) + "::" + spelling;
                if (classes.ContainsKey(candidate)) return candidate;
            }
            throw new MetadataException($"{owner}: base/reference type '{spelling}' is not reflected in this unit",
                hint: "Include the reflected base/reference definition in this generation unit.").At(site);
        }
        foreach (ClassModel model in classes.Values)
        {
            model.Base = Resolve(model.Base, model.Name, model.Site);
            foreach (FieldModel field in model.OwnFields)
            {
                if (Scalars.TryGetValue(field.NativeSpelling, out string? kind)) field.Kind = kind;
                else if (field.NativeSpelling.StartsWith("rocks::ObjectRef<", StringComparison.Ordinal) && field.NativeSpelling.EndsWith('>'))
                {
                    string target = field.NativeSpelling[17..^1].Trim();
                    field.Reference = Resolve(target.Length == 0 ? "rocks::Object" : target, model.Name, field.Site);
                    field.Kind = "ObjectReference";
                }
                else throw new MetadataException($"{field.Owner}::{field.Name}: unsupported field type '{field.NativeSpelling}'",
                    hint: "Use a supported scalar, Guid, String or ObjectRef field; raw pointers/containers/cv-qualified fields need an explicit codec extension.").At(field.Site);
            }
        }
        List<ClassModel> ordered = [];
        HashSet<string> visiting = new(StringComparer.Ordinal);
        HashSet<string> visited = new(StringComparer.Ordinal);
        void Order(ClassModel model)
        {
            if (visited.Contains(model.Name)) return;
            if (!visiting.Add(model.Name)) throw new MetadataException($"{model.Name}: inheritance cycle").At(model.Site);
            List<FieldModel> inherited = [];
            if (model.Base != "rocks::Object")
            {
                Order(classes[model.Base]);
                inherited.AddRange(classes[model.Base].Fields);
            }
            model.Fields = inherited.Concat(model.OwnFields).OrderBy(field => field.Id, StringComparer.Ordinal).ToList();
            if (model.Fields.Count > 4096) throw new MetadataException($"{model.Name}: inherited field count exceeds 4096").At(model.Site);
            visiting.Remove(model.Name);
            visited.Add(model.Name);
            ordered.Add(model);
        }
        foreach (ClassModel model in classes.Values.OrderBy(model => model.Name, StringComparer.Ordinal)) Order(model);
        return new(classes, ordered);
    }
}
