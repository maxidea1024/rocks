
namespace Rocks.MetaCompiler;

// 빌드와 별도인 명시적 저작 명령이다. GUID는 도구가 발급한다.
internal static class IdentityCommands
{
    public static int Run(string[] args)
    {
        if (args.Length == 0)
        {
            throw new ArgumentException("Expected ids add, rename, retire, move, verify, verify-set or merge-check.");
        }

        var options = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        for (int index = 1; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length || !args[index].StartsWith("--", StringComparison.Ordinal) ||
                string.IsNullOrWhiteSpace(args[index + 1]))
            {
                throw new ArgumentException("Expected option/value pairs for ids command.");
            }

            string key = args[index][2..];
            if (!options.TryGetValue(key, out var list))
            {
                options[key] = list = [];
            }

            list.Add(args[index + 1]);
        }

        void Allow(params string[] names)
        {
            foreach (string key in options.Keys)
            {
                if (!names.Contains(key, StringComparer.Ordinal))
                {
                    throw new ArgumentException("Unknown ids option --" + key);
                }
            }
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
                    if (types.Length + fields.Length == 0)
                    {
                        throw new ArgumentException("Specify at least one --type or --field.");
                    }

                    StableIdManifest.Update(path, true, manifest =>
                    {
                        foreach (string name in types)
                        {
                            manifest.Register("type", name);
                        }

                        foreach (string name in fields)
                        {
                            manifest.Register("field", name);
                        }

                        return 0;
                    });
                    Console.WriteLine($"Registered {types.Length} types and {fields.Length} fields in {path}.");
                    return 0;
                }
            case "rename":
                {
                    Allow("manifest", "kind", "old", "new");
                    string path = ManifestPath(One("manifest")), kind = One("kind"), oldName = One("old"), newName = One("new");
                    StableIdManifest.Update(path, false, manifest =>
                    {
                        manifest.Rename(kind, oldName, newName);
                        return 0;
                    });
                    Console.WriteLine($"Renamed {kind} {oldName} to {newName}, preserving its ID.");
                    return 0;
                }
            case "retire":
                {
                    Allow("manifest", "kind", "name");
                    string path = ManifestPath(One("manifest")), kind = One("kind"), name = One("name");
                    StableIdManifest.Update(path, false, manifest =>
                    {
                        manifest.Retire(kind, name);
                        return 0;
                    });
                    Console.WriteLine($"Retired {kind} {name}; its ID remains reserved.");
                    return 0;
                }
            case "move":
                {
                    // 타입과 그 필드 항목을 ID 그대로 다른 명세로 옮긴다(모듈 이전·명세 위치 변경). 대상에 먼저 붙이고
                    // 원본에서 뗀다 — 중간에 멈추면 두 명세에 같은 ID가 남아 verify-set이 실패하므로 같은 명령을 다시 실행한다.
                    Allow("from", "to", "type");
                    string from = ManifestPath(One("from"));
                    string to = ManifestPath(One("to"));
                    if (Path.GetFullPath(from).Equals(Path.GetFullPath(to), OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                    {
                        throw new ArgumentException("--from and --to must be different manifests.");
                    }

                    string[] types = Many("type");
                    if (types.Length == 0)
                    {
                        throw new ArgumentException("Specify at least one --type.");
                    }

                    // 중첩 타입이 남아 있으면 바깥 타입을 뗄 수 없으므로 깊은 이름부터 뗀다(지정 순서와 무관).
                    types = types.OrderByDescending(type => type.Split("::").Length).ThenBy(type => type, StringComparer.Ordinal).ToArray();
                    var moved = new List<StableIdEntry>();
                    StableIdManifest source = StableIdManifest.Load(from);
                    foreach (string type in types)
                    {
                        moved.AddRange(source.Detach(type));
                    }

                    StableIdManifest.Update(to, true, manifest =>
                    {
                        manifest.Attach(moved);
                        return 0;
                    });
                    StableIdManifest.Update(from, false, manifest =>
                    {
                        foreach (string type in types)
                        {
                            manifest.Detach(type);
                        }

                        return 0;
                    });
                    Console.WriteLine($"Moved {types.Length} types ({moved.Count} entries) from {from} to {to}, preserving IDs.");
                    return 0;
                }
            case "verify":
                {
                    Allow("manifest", "inventory");
                    IdentityInventory.Verify(ManifestPath(One("manifest")), Many("inventory"));
                    Console.WriteLine("Stable identity inventory: PASS");
                    return 0;
                }
            case "verify-set":
                {
                    // 타깃의 모든 명세(SDK 동봉 명세 포함)에서 활성·은퇴 ID를 모아 두 파일에 같은 ID가 있으면 실패한다.
                    Allow("manifest");
                    string[] manifests = Many("manifest");
                    if (manifests.Length == 0)
                    {
                        throw new ArgumentException("Specify at least one --manifest.");
                    }

                    var loaded = manifests.Select(ManifestPath)
                        .DistinctBy(Path.GetFullPath, OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal)
                        .Select(path => (path, StableIdManifest.Load(path))).ToArray();
                    IReadOnlyList<string> failures = StableIdManifest.SetFailures(loaded);
                    foreach (string failure in failures)
                    {
                        Console.Error.WriteLine($"{manifests[0]}:1: error RMC2101: stable identity: {failure}");
                    }

                    if (failures.Count != 0)
                    {
                        Console.Error.WriteLine("hint: Keep each ID and name in exactly one module manifest; move a type with ids move instead of copying entries.");
                        return 1;
                    }

                    Console.WriteLine($"Stable identity set ({loaded.Sum(item => item.Item2.Entries.Count)} IDs in {loaded.Length} manifests): PASS");
                    return 0;
                }
            case "merge-check":
                {
                    // 각 측면에 그 리비전의 모든 명세를 넘긴다(여러 번 지정). 측면마다 ID 기준으로 합쳐 비교하므로 명세를
                    // 다른 경로로 옮겨도 ID가 사라진 것으로 보지 않는다. 명세가 없는 측면은 --<측면> none이다.
                    Allow("base", "ours", "theirs", "merged");
                    var sides = new Dictionary<string, (string Path, StableIdManifest Manifest)[]>(StringComparer.Ordinal);
                    var setFailures = new List<string>();
                    foreach (string key in new[] { "base", "ours", "theirs", "merged" })
                    {
                        if (Many(key).Length == 0)
                        {
                            throw new ArgumentException($"Specify --{key} at least once (none for no manifests).");
                        }

                        // 같은 파일을 두 번 넘겨도 한 번만 센다(파일 사이 중복과 구분한다).
                        sides[key] = Many(key).Where(path => path != "none")
                            .DistinctBy(Path.GetFullPath, OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal)
                            .Select(path => (path, StableIdManifest.Load(path))).ToArray();
                        setFailures.AddRange(StableIdManifest.SetFailures(sides[key]).Select(failure => $"{key}: {failure}"));
                    }

                    // 한 리비전 안에서 ID·이름은 한 명세에만 있다. 합친 뷰를 만들기 전에 파일과 함께 보고한다.
                    if (setFailures.Count != 0)
                    {
                        foreach (string failure in setFailures)
                        {
                            Console.Error.WriteLine("identity merge: " + failure);
                        }

                        return 1;
                    }

                    StableIdManifest Side(string key) => StableIdManifest.Combine(sides[key].Select(side => side.Manifest));
                    var failures = StableIdMergeValidator.Validate(Side("base"), Side("ours"), Side("theirs"), Side("merged"));
                    foreach (string failure in failures)
                    {
                        Console.Error.WriteLine("identity merge: " + failure);
                    }

                    if (failures.Count != 0)
                    {
                        return 1;
                    }

                    Console.WriteLine("Stable identity merge: PASS");
                    return 0;
                }
            default:
                throw new ArgumentException("Unknown ids command: " + args[0]);
        }
    }

    private static string ManifestPath(string path)
    {
        if (!path.EndsWith(".rocksids.json", StringComparison.Ordinal))
        {
            throw new ArgumentException("Identity manifest must end in .rocksids.json for history checks.");
        }

        return path;
    }
}
