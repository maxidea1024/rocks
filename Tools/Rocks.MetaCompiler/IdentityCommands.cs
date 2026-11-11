
namespace Rocks.MetaCompiler;

// 빌드와 별도인 명시적 저작 명령이다. GUID는 도구가 발급한다.
internal static class IdentityCommands
{
    public static int Run(string[] args)
    {
        if (args.Length == 0) throw new ArgumentException("Expected ids add, rename, retire, verify or merge-check.");
        var options = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length || !args[index].StartsWith("--", StringComparison.Ordinal) ||
                string.IsNullOrWhiteSpace(args[index + 1]))
                throw new ArgumentException("Expected option/value pairs for ids command.");
            string key = args[index][2..];
            if (!options.TryGetValue(key, out var list)) options[key] = list = [];
            list.Add(args[index + 1]);
        }
        void Allow(params string[] names)
        {
            foreach (string key in options.Keys)
                if (!names.Contains(key, StringComparer.Ordinal)) throw new ArgumentException("Unknown ids option --" + key);
        }
        string One(string key) => options.TryGetValue(key, out var values) && values.Count == 1
            ? values[0] : throw new ArgumentException("Expected exactly one --" + key);
        string[] Many(string key) => options.TryGetValue(key, out var values) ? values.ToArray() : [];
        switch (args[0])
        {
            case "add":
            {
                Allow("manifest", "type", "field");
                string path = ManifestPath(One("manifest"));
                string[] types = Many("type");
                string[] fields = Many("field");
                if (types.Length + fields.Length == 0) throw new ArgumentException("Specify at least one --type or --field.");
                StableIdManifest.Update(path, true, manifest =>
                {
                    foreach (string name in types) manifest.Register("type", name);
                    foreach (string name in fields) manifest.Register("field", name);
                    return 0;
                });
                Console.WriteLine($"Registered {types.Length} types and {fields.Length} fields in {path}.");
                return 0;
            }
            case "rename":
            {
                Allow("manifest", "kind", "old", "new");
                string path = ManifestPath(One("manifest")), kind = One("kind"), oldName = One("old"), newName = One("new");
                StableIdManifest.Update(path, false, manifest => { manifest.Rename(kind, oldName, newName); return 0; });
                Console.WriteLine($"Renamed {kind} {oldName} to {newName}, preserving its ID.");
                return 0;
            }
            case "retire":
            {
                Allow("manifest", "kind", "name");
                string path = ManifestPath(One("manifest")), kind = One("kind"), name = One("name");
                StableIdManifest.Update(path, false, manifest => { manifest.Retire(kind, name); return 0; });
                Console.WriteLine($"Retired {kind} {name}; its ID remains reserved.");
                return 0;
            }
            case "verify":
            {
                Allow("manifest", "inventory");
                IdentityInventory.Verify(ManifestPath(One("manifest")), Many("inventory"));
                Console.WriteLine("Stable identity inventory: PASS");
                return 0;
            }
            case "merge-check":
            {
                Allow("base", "ours", "theirs", "merged");
                var failures = StableIdMergeValidator.Validate(StableIdManifest.Load(One("base")),
                    StableIdManifest.Load(One("ours")), StableIdManifest.Load(One("theirs")),
                    StableIdManifest.Load(One("merged")));
                foreach (string failure in failures) Console.Error.WriteLine("identity merge: " + failure);
                if (failures.Count != 0) return 1;
                Console.WriteLine("Stable identity merge: PASS");
                return 0;
            }
            default: throw new ArgumentException("Unknown ids command: " + args[0]);
        }
    }

    private static string ManifestPath(string path)
    {
        if (!path.EndsWith(".rocksids.json", StringComparison.Ordinal))
            throw new ArgumentException("Identity manifest must end in .rocksids.json for history checks.");
        return path;
    }
}
