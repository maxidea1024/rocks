using System.Text.Json.Nodes;
using System.Text.Json;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class ApplePropertyLocationTests
{
    public static void Register(TestSuite suite)
    {
        suite.Add("Meta.PropertyLocationAcceptsAppleAndLlvmShapes", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("Field.h");
            const string source = "ROCKS_PROPERTY()\n// a comment\n   int key;\n";
            File.WriteAllText(path, source);
            var apple = Field(path, source, source.IndexOf("int key", StringComparison.Ordinal));
            Validate(path, apple);
            var llvm = Field(path, source, source.IndexOf("int key", StringComparison.Ordinal), llvm: true);
            Validate(path, llvm);
        });

        suite.Add("Meta.ApplePropertyLocationRejectsInterveningTokensAndWrappers", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("Field.h");
            foreach (string source in new[]
            {
                "ROCKS_PROPERTY()\n#include \"other.h\"\nint key;\n",
                "ROCKS_PROPERTY()\n[[maybe_unused]] int key;\n",
                "ROCKS_PROPERTY()\nFORWARDED int key;\n",
                "ROCKS_PROPERTY()\n// line splice \\\nint key;\n"
            })
            {
                File.WriteAllText(path, source);
                var field = Field(path, source, source.IndexOf("int key", StringComparison.Ordinal));
                Verify.Throws<MetadataException>(() => Validate(path, field));
            }

            const string wrapper = "WRAPPER() int key;\n";
            File.WriteAllText(path, wrapper);
            var wrapped = Field(path, wrapper, wrapper.IndexOf("int key", StringComparison.Ordinal), marker: 0);
            Verify.Throws<MetadataException>(() => Validate(path, wrapped));
        });

        suite.Add("Meta.ApplePropertyLocationRejectsMissingAndConflictingProvenance", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("Field.h");
            string other = temporary.FilePath("Other.h");
            const string source = "ROCKS_PROPERTY() int key;\n";
            File.WriteAllText(path, source);
            File.WriteAllText(other, source);
            int type = source.IndexOf("int key", StringComparison.Ordinal);
            foreach (JsonObject mutation in new[]
            {
                Mutate(path, source, type, root => root["inner"]![0]!["range"]!["begin"]!["expansionLoc"]!.AsObject().Remove("file")),
                Mutate(path, source, type, root => root["inner"]![0]!["range"]!["begin"]!["expansionLoc"]!["file"] = other),
                Mutate(path, source, type, root => root["range"]!["begin"]!["file"] = other),
                Mutate(path, source, type, root => root["range"]!["begin"]!["includedFrom"] = new JsonObject { ["file"] = "different-parent.h" }),
                Mutate(path, source, type, root => root["loc"]!.AsObject().Remove("file")),
                Mutate(path, source, type, root => root["range"]!["begin"]!["offset"] = 0),
            })
            {
                Verify.Throws<MetadataException>(() => Validate(path, mutation));
            }
        });
    }

    private static void Validate(string path, JsonObject field)
    {
        using JsonDocument document = JsonDocument.Parse(field.ToJsonString());
        new ClangAstParser(path).VerifyPropertySourceForTest(document.RootElement);
    }

    private static JsonObject Mutate(string path, string source, int type, Action<JsonObject> mutate)
    {
        JsonObject value = Field(path, source, type);
        mutate(value);
        return value;
    }

    private static JsonObject Field(string path, string source, int type, bool llvm = false, int marker = 0)
    {
        int declaration = source.IndexOf("key", StringComparison.Ordinal);
        var expansion = new JsonObject { ["file"] = path, ["offset"] = marker };
        var begin = llvm ? new JsonObject { ["expansionLoc"] = expansion.DeepClone() } : new JsonObject { ["offset"] = type };
        return new JsonObject
        {
            ["id"] = "field-id",
            ["kind"] = "FieldDecl",
            ["loc"] = new JsonObject { ["file"] = path, ["offset"] = declaration },
            ["range"] = new JsonObject { ["begin"] = begin },
            ["inner"] = new JsonArray(new JsonObject
            {
                ["kind"] = "AnnotateAttr",
                ["range"] = new JsonObject { ["begin"] = new JsonObject { ["expansionLoc"] = expansion } },
                ["inner"] = new JsonArray(new JsonObject { ["kind"] = "StringLiteral", ["value"] = "\"rocks.field.arguments\"" })
            })
        };
    }
}
