using System.Text.Json;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class StableIdMergeTests
{
    private const string FirstId = "10000000000000000000000000000001";
    private const string SecondId = "10000000000000000000000000000002";
    private const string FieldId = "20000000000000000000000000000001";

    private static StableIdEntry Type(string name, string id = FirstId, bool retired = false, params string[] aliases) =>
        new("type", name, id, aliases, retired);

    private static StableIdEntry Field(string name, params string[] aliases) =>
        new("field", name, FieldId, aliases, false);

    public static void Register(TestSuite suite)
    {
        suite.Add("IdentityMerge.DisjointAdditionsAndSingleRename", () =>
        {
            StableIdEntry[] ancestor = [Type("sample::Old"), Field("sample::Old::health")];
            StableIdEntry[] ours = [Type("sample::New", FirstId, false, "sample::Old"),
                Field("sample::New::health", "sample::Old::health")];
            StableIdEntry[] theirs = [.. ancestor, Type("sample::Extra", SecondId)];
            StableIdEntry[] merged = [.. ours, Type("sample::Extra", SecondId)];
            Verify.Equal(0, Compare(ancestor, ours, theirs, merged).Count, "valid merge rejected");
        });
        suite.Add("IdentityMerge.LostIdsAndNameHistoryAreRejected", () =>
        {
            StableIdEntry[] ancestor = [Type("sample::Old")];
            StableIdEntry[] renamed = [Type("sample::New", FirstId, false, "sample::Old")];
            Has(Compare(ancestor, renamed, ancestor, []), "disappeared");
            Has(Compare(ancestor, renamed, ancestor, [Type("sample::New")]), "lost reserved name");
            Has(Compare(ancestor, ancestor, ancestor, [Type("sample::Old", SecondId)]), "disappeared");
            StableIdEntry[] reversed = [Type("sample::Old", FirstId, false, "sample::New")];
            Has(Compare(ancestor, renamed, ancestor, reversed), "discarded the ours rename");
            Has(Compare(ancestor, ancestor, renamed, reversed), "discarded the theirs rename");
        });
        suite.Add("IdentityMerge.ConcurrentNameCollisionAndConflictingRename", () =>
        {
            StableIdEntry[] empty = [];
            StableIdEntry[] one = [Type("sample::Same", FirstId)];
            StableIdEntry[] two = [Type("sample::Same", SecondId)];
            Has(Compare(empty, one, two, one), "name collision");
            StableIdEntry[] ancestor = [Type("sample::Old")];
            StableIdEntry[] left = [Type("sample::Left", FirstId, false, "sample::Old")];
            StableIdEntry[] right = [Type("sample::Right", FirstId, false, "sample::Old")];
            StableIdEntry[] merged = [Type("sample::Left", FirstId, false, "sample::Old", "sample::Right")];
            Has(Compare(ancestor, left, right, merged), "conflicting rename");
        });
        suite.Add("IdentityMerge.RetirementAndUnreviewedRenameAreRejected", () =>
        {
            StableIdEntry[] ancestor = [Type("sample::Old")];
            StableIdEntry[] retired = [Type("sample::Old", FirstId, true)];
            Has(Compare(ancestor, retired, ancestor, ancestor), "retired ID");
            StableIdEntry[] renamed = [Type("sample::New", FirstId, false, "sample::Old")];
            StableIdEntry[] mergedRetired = [Type("sample::New", FirstId, true, "sample::Old")];
            Has(Compare(ancestor, retired, renamed, mergedRetired), "rename conflicts with retirement");
            StableIdEntry[] unexpected = [Type("sample::Other", FirstId, false, "sample::Old")];
            Has(Compare(ancestor, ancestor, ancestor, unexpected), "unreviewed rename");
        });
        suite.Add("IdentityMerge.FieldOwnerIdentityCannotChange", () =>
        {
            StableIdEntry[] ancestor = [Type("sample::A"), Type("sample::B", SecondId), Field("sample::A::value")];
            StableIdEntry[] moved = [Type("sample::A"), Type("sample::B", SecondId),
                Field("sample::B::value")];
            Has(Compare(ancestor, moved, ancestor, moved), "changed or lost its owning type");
        });
        suite.Add("IdentityMerge.NewIdWithDifferentParentNamesIsRejected", () =>
        {
            StableIdEntry[] left = [Type("sample::A")];
            StableIdEntry[] right = [Type("sample::B", FirstId, false, "sample::A")];
            Has(Compare([], left, right, right), "independently added ID");
        });
        suite.Add("IdentityMerge.TypeRetirementConflictsWithOtherBranchFieldChanges", () =>
        {
            StableIdEntry[] ancestor = [Type("sample::A")];
            StableIdEntry[] retired = [Type("sample::A", FirstId, true)];
            StableIdEntry[] extended = [Type("sample::A"), Field("sample::A::value")];
            StableIdEntry[] merged = [Type("sample::A", FirstId, true),
                new("field", "sample::A::value", FieldId, [], true)];
            Has(Compare(ancestor, retired, extended, merged), "type retirement conflicts with member changes");
            Has(Compare(ancestor, extended, retired, merged), "type retirement conflicts with member changes");
            Verify.Equal(0, Compare(ancestor, retired, ancestor, retired).Count,
                "one-sided retirement without field changes rejected");
        });
        suite.Add("IdentityMerge.SparksMemberNames", () =>
        {
            // Sparks 이름 키(`<패키지>/<모듈>.<타입>.<멤버>`)와 멤버 kind 전부를 C++ 이름과 같은 규칙으로 본다
            // (Docs/Design/SparksBytecode.ko.md 6.1절).
            const string Door = "Game/Doors/Door.Door";
            const string Gate = "Game/Doors/Door.Gate";
            const string MemberId = "30000000000000000000000000000001";
            StableIdEntry[] ancestor = [Type(Door), Field(Door + ".Open"),
                new("subobject", Door + ".Hinge", MemberId, [], false)];
            StableIdEntry[] renamed = [Type(Gate, FirstId, false, Door), Field(Gate + ".Open", Door + ".Open"),
                new("subobject", Gate + ".Hinge", MemberId, [Door + ".Hinge"], false)];
            StableIdEntry[] extra = [.. ancestor, new("enumCase", "Game/Doors/Door.State.Closed", SecondId, [], false),
                Type("Game/Doors/Door.State", "40000000000000000000000000000001")];
            StableIdEntry[] merged = [.. renamed, new("enumCase", "Game/Doors/Door.State.Closed", SecondId, [], false),
                Type("Game/Doors/Door.State", "40000000000000000000000000000001")];
            Verify.Equal(0, Compare(ancestor, renamed, extra, merged).Count, "valid Sparks merge rejected");
            // 멤버가 다른 타입으로 옮겨 가면(주인 ID가 바뀜) subobject도 거부한다.
            StableIdEntry[] two = [Type(Door), Type(Gate, SecondId), Field(Door + ".Open"),
                new("subobject", Door + ".Hinge", MemberId, [], false)];
            StableIdEntry[] moved = [Type(Door), Type(Gate, SecondId), Field(Door + ".Open"),
                new("subobject", Gate + ".Hinge", MemberId, [], false)];
            Has(Compare(two, moved, two, moved), "subobject ID " + MemberId + " changed or lost its owning type");
            // 한쪽이 타입을 은퇴하는 동안 다른 쪽이 function 멤버를 더했다.
            StableIdEntry[] retired = [Type(Door, FirstId, true), new("field", Door + ".Open", FieldId, [], true),
                new("subobject", Door + ".Hinge", MemberId, [], true)];
            StableIdEntry[] extended = [.. ancestor, new("function", Door + ".on(Owner.Destroyed)", SecondId, [], false)];
            StableIdEntry[] mergedRetired = [.. retired, new("function", Door + ".on(Owner.Destroyed)", SecondId, [], true)];
            Has(Compare(ancestor, retired, extended, mergedRetired), "type retirement conflicts with member changes");
        });
    }

    private static void Has(IReadOnlyList<string> failures, string fragment) =>
        Verify.That(failures.Any(failure => failure.Contains(fragment, StringComparison.Ordinal)),
            "expected merge diagnostic '" + fragment + "': " + string.Join("; ", failures));

    private static IReadOnlyList<string> Compare(StableIdEntry[] ancestor, StableIdEntry[] ours,
        StableIdEntry[] theirs, StableIdEntry[] merged)
    {
        using TemporaryDirectory temporary = new();
        StableIdManifest Load(string filename, StableIdEntry[] entries)
        {
            temporary.Write(filename, JsonSerializer.Serialize(new
            {
                schema = 1,
                entries = entries.Select(entry =>
                new { kind = entry.Kind, name = entry.Name, id = entry.Id, aliases = entry.Aliases, retired = entry.Retired })
            }));
            return StableIdManifest.Load(temporary.FilePath(filename));
        }

        return StableIdMergeValidator.Validate(Load("ancestor.json", ancestor), Load("ours.json", ours),
            Load("theirs.json", theirs), Load("merged.json", merged));
    }
}
