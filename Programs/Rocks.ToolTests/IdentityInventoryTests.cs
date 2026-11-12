using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class IdentityInventoryTests
{
    public static void Register(TestSuite suite)
    {
        suite.Add("IdentityInventory.RootManifestChangesSourceEvidence", () =>
        {
            using TemporaryDirectory temp = new();
            temp.Write("root.rocksids.json", "before");
            var before = SourceSnapshot.Capture(temp.Root);
            temp.Write("root.rocksids.json", "after");
            Verify.That(before != SourceSnapshot.Capture(temp.Root), "root identity manifest escaped source evidence");
            temp.Write("ProjectData/Schema/nested.rocksids.json", "before");
            var nested = SourceSnapshot.Capture(temp.Root);
            temp.Write("ProjectData/Schema/nested.rocksids.json", "after");
            Verify.That(nested != SourceSnapshot.Capture(temp.Root), "nested identity manifest escaped source evidence");
        });
        suite.Add("IdentityInventory.DetectsMissingRenamesAndDuplicateDefinitions", () =>
        {
            using TemporaryDirectory temp = new();
            string manifestPath = temp.FilePath("test.rocksids.json");
            StableIdManifest.Update(manifestPath, true, manifest =>
            {
                manifest.Register("type", "demo::Actor");
                manifest.Register("field", "demo::Actor::health");
                return 0;
            });
            var ids = StableIdManifest.Load(manifestPath);
            temp.Write("Actor.h", "// fixture\n");
            temp.Write("Other.h", "// another definition\n");
            string source = temp.FilePath("Actor.h"), other = temp.FilePath("Other.h");
            var type = new { kind = "type", name = "demo::Actor", id = ids.Resolve("type", "demo::Actor"), file = source, line = 1 };
            var field = new { kind = "field", name = "demo::Actor::health", id = ids.Resolve("field", "demo::Actor::health"), file = source, line = 2 };
            string first = temp.FilePath("first.json"), second = temp.FilePath("second.json");
            void Write(string path, params object[] declarations) => File.WriteAllText(path,
                JsonSerializer.Serialize(new { schema = 1, declarations }));
            Write(first, type, field);
            Write(second, type, field);
            Verify.Throws<InvalidDataException>(() => IdentityInventory.Verify(manifestPath, [first, second])); // 동일 선언의 중복 unit도 현재 거부
            Write(first, type);
            Verify.Throws<InvalidDataException>(() => IdentityInventory.Verify(manifestPath, [first]));
            Write(first, type, field);
            Write(second, new { kind = "type", name = "demo::Actor", id = type.id, file = other, line = 1 });
            Verify.Throws<InvalidDataException>(() => IdentityInventory.Verify(manifestPath, [first, second]));
            Write(second, new { kind = "type", name = "demo::Actor", id = field.id, file = source, line = 1 });
            Verify.Throws<InvalidDataException>(() => IdentityInventory.Verify(manifestPath, [first, second]));
            StableIdManifest.Update(manifestPath, false, manifest => { manifest.Retire("field", "demo::Actor::health"); return 0; });
            Write(first, type);
            IdentityInventory.Verify(manifestPath, [first]);
        });
    }
}
