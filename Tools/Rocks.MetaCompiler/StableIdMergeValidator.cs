namespace Rocks.MetaCompiler;

// 병합 결과가 양쪽에서 이미 사용된 식별자와 이름 이력을 모두 보존하는지 검사한다.
// 네 입력은 호출자가 선택한 공통 조상, 두 부모 및 병합 결과의 검증된 manifest다.
// 이 비교만으로 호출자가 실제 Git 부모를 넘겼는지, 실수로 이미 손실된 과거가 있는지는 증명할 수 없다.
internal static class StableIdMergeValidator
{
    public static IReadOnlyList<string> Validate(StableIdManifest ancestor, StableIdManifest ours,
        StableIdManifest theirs, StableIdManifest merged)
    {
        ArgumentNullException.ThrowIfNull(ancestor);
        ArgumentNullException.ThrowIfNull(ours);
        ArgumentNullException.ThrowIfNull(theirs);
        ArgumentNullException.ThrowIfNull(merged);
        var failures = new SortedSet<string>(StringComparer.Ordinal);
        Dictionary<string, StableIdEntry> baseIds = ById(ancestor);
        Dictionary<string, StableIdEntry> ourIds = ById(ours);
        Dictionary<string, StableIdEntry> theirIds = ById(theirs);
        Dictionary<string, StableIdEntry> mergedIds = ById(merged);
        Dictionary<string, List<StableIdEntry>> ourFields = FieldsByOwner(ours);
        Dictionary<string, List<StableIdEntry>> theirFields = FieldsByOwner(theirs);

        Preserve(ancestor, ours, baseIds, ourIds, "ancestor -> ours");
        Preserve(ancestor, theirs, baseIds, theirIds, "ancestor -> theirs");
        Preserve(ours, merged, ourIds, mergedIds, "ours -> merge");
        Preserve(theirs, merged, theirIds, mergedIds, "theirs -> merge");

        // 다른 브랜치가 각각 같은 이름에 새 ID를 만든 경우, 한쪽을 골라 버리면 그쪽의
        // 저장 데이터가 손실된다. 현재 이름과 과거 alias 모두 비교한다.
        var theirNames = ByName(theirs);
        foreach (StableIdEntry entry in ours.Entries)
            foreach (string name in Names(entry))
                if (theirNames.TryGetValue((entry.Kind, name), out string? otherId) &&
                    !StringComparer.Ordinal.Equals(entry.Id, otherId))
                    failures.Add($"name collision: {entry.Kind} {name} has different IDs in the two parents ({entry.Id}, {otherId})");

        foreach ((string id, StableIdEntry left) in ourIds)
        {
            if (!theirIds.TryGetValue(id, out StableIdEntry? right)) continue;
            if (!StringComparer.Ordinal.Equals(left.Kind, right.Kind))
            {
                failures.Add($"ID {id} has different kinds in the two parents");
                continue;
            }
            if (baseIds.TryGetValue(id, out StableIdEntry? prior))
            {
                bool leftRenamed = !StringComparer.Ordinal.Equals(left.Name, prior.Name);
                bool rightRenamed = !StringComparer.Ordinal.Equals(right.Name, prior.Name);
                if (leftRenamed && rightRenamed && !StringComparer.Ordinal.Equals(left.Name, right.Name))
                    failures.Add($"conflicting rename for {left.Kind} ID {id}: {left.Name} versus {right.Name}");
                if ((left.Retired && !prior.Retired && rightRenamed) ||
                    (right.Retired && !prior.Retired && leftRenamed))
                    failures.Add($"rename conflicts with retirement for {left.Kind} ID {id}");
                // 한쪽에서 타입을 은퇴하는 동안 다른 쪽이 그 타입의 필드를 만들거나 바꿨다면
                // 새 필드까지 은퇴시켜 병합하는 것은 조용한 데이터 손실이다.
                if (left.Kind == "type" && !prior.Retired &&
                    ((left.Retired && HasFieldChanges(id, theirFields, baseIds)) ||
                     (right.Retired && HasFieldChanges(id, ourFields, baseIds))))
                    failures.Add($"type retirement conflicts with field changes for type ID {id}");
                if (mergedIds.TryGetValue(id, out StableIdEntry? result))
                {
                    if (leftRenamed && !rightRenamed && !StringComparer.Ordinal.Equals(result.Name, left.Name))
                        failures.Add($"merge discarded the ours rename for {left.Kind} ID {id}");
                    if (rightRenamed && !leftRenamed && !StringComparer.Ordinal.Equals(result.Name, right.Name))
                        failures.Add($"merge discarded the theirs rename for {left.Kind} ID {id}");
                }
            }
            else if (!StringComparer.Ordinal.Equals(left.Name, right.Name))
                failures.Add($"independently added ID {id} has different names in the two parents");

        }
        foreach ((string id, StableIdEntry result) in mergedIds)
        {
            bool hadLeft = ourIds.TryGetValue(id, out StableIdEntry? left);
            bool hadRight = theirIds.TryGetValue(id, out StableIdEntry? right);
            if ((hadLeft || hadRight) &&
                (!hadLeft || !StringComparer.Ordinal.Equals(result.Name, left!.Name)) &&
                (!hadRight || !StringComparer.Ordinal.Equals(result.Name, right!.Name)))
                failures.Add($"merge introduced an unreviewed rename for {result.Kind} ID {id}: {result.Name}");
        }
        return failures.ToArray();

        void Preserve(StableIdManifest from, StableIdManifest to, Dictionary<string, StableIdEntry> before,
            Dictionary<string, StableIdEntry> after, string edge)
        {
            var oldNames = ByName(from);
            var newNames = ByName(to);
            foreach ((string id, StableIdEntry old) in before)
            {
                if (!after.TryGetValue(id, out StableIdEntry? next))
                {
                    failures.Add($"{edge}: ID {id} ({old.Kind} {old.Name}) disappeared or was replaced");
                    continue;
                }
                if (!StringComparer.Ordinal.Equals(old.Kind, next.Kind))
                    failures.Add($"{edge}: ID {id} changed kind from {old.Kind} to {next.Kind}");
                if (old.Retired && !next.Retired)
                    failures.Add($"{edge}: retired ID {id} was reused");
                if (old.Kind == "field" && next.Kind == "field")
                {
                    string oldOwner = Owner(old.Name);
                    string newOwner = Owner(next.Name);
                    if (!oldNames.TryGetValue(("type", oldOwner), out string? oldOwnerId) ||
                        !newNames.TryGetValue(("type", newOwner), out string? newOwnerId) ||
                        !StringComparer.Ordinal.Equals(oldOwnerId, newOwnerId))
                        failures.Add($"{edge}: field ID {id} changed or lost its owning type");
                }
                HashSet<string> nextNames = Names(next).ToHashSet(StringComparer.Ordinal);
                foreach (string name in Names(old))
                    if (!nextNames.Contains(name))
                        failures.Add($"{edge}: ID {id} lost reserved name {name}");
            }
        }
    }

    private static Dictionary<string, StableIdEntry> ById(StableIdManifest manifest) =>
        manifest.Entries.ToDictionary(entry => entry.Id, StringComparer.Ordinal);

    private static Dictionary<(string Kind, string Name), string> ByName(StableIdManifest manifest)
    {
        var names = new Dictionary<(string Kind, string Name), string>();
        foreach (StableIdEntry entry in manifest.Entries)
            foreach (string name in Names(entry))
                names.Add((entry.Kind, name), entry.Id);
        return names;
    }

    private static IEnumerable<string> Names(StableIdEntry entry) => entry.Aliases.Prepend(entry.Name);

    private static Dictionary<string, List<StableIdEntry>> FieldsByOwner(StableIdManifest manifest)
    {
        var names = ByName(manifest);
        var result = new Dictionary<string, List<StableIdEntry>>(StringComparer.Ordinal);
        foreach (StableIdEntry field in manifest.Entries.Where(entry => entry.Kind == "field"))
        {
            if (!names.TryGetValue(("type", Owner(field.Name)), out string? typeId))
                throw new InvalidDataException($"field {field.Name} has no owner");
            if (!result.TryGetValue(typeId, out List<StableIdEntry>? fields))
                result.Add(typeId, fields = []);
            fields.Add(field);
        }
        return result;
    }

    private static bool HasFieldChanges(string ownerId, Dictionary<string, List<StableIdEntry>> fields,
        Dictionary<string, StableIdEntry> ancestor)
    {
        if (!fields.TryGetValue(ownerId, out List<StableIdEntry>? owned)) return false;
        foreach (StableIdEntry field in owned)
        {
            if (!ancestor.TryGetValue(field.Id, out StableIdEntry? prior) ||
                prior.Kind != field.Kind || prior.Name != field.Name || prior.Retired != field.Retired ||
                !prior.Aliases.ToHashSet(StringComparer.Ordinal).SetEquals(field.Aliases))
                return true;
        }
        return false;
    }

    private static string Owner(string name)
    {
        int separator = name.LastIndexOf("::", StringComparison.Ordinal);
        return separator < 0 ? "" : name[..separator];
    }
}
