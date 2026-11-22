using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 리플렉션 B9m 상속 bitfield 시험이다(Docs/Design/ReflectionExtensions.ko.md 14절).
internal static partial class ModuleMetaTests
{
    private const string BitfieldParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::uint8 flag : 1 = 0;\n};\n}\n";

    // 다른 모듈 base의 bitfield bool 필드는 descriptor의 `"bitfield": true`로 전해져, 파생 모듈의 함수표가 base의 생성 접근자를
    // 쓴다(생성 헤더는 MetaCompiler 검증 컴파일을 통과해야 게시된다).
    private static void RegisterBitfields(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.ImportedBitfieldUsesBaseAccessors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", BitfieldParent);
            layout.Manifest("Lower", [], ["probe::Parent::flag"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "Lower module: " + lower.StdErr);
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                JsonElement flag = descriptor.RootElement.GetProperty("types").EnumerateArray()
                    .Single(type => type.GetProperty("name").GetString() == "probe::Parent").GetProperty("fields").EnumerateArray()
                    .Single(field => field.GetProperty("name").GetString() == "flag");
                Verify.That(flag.GetProperty("bitfield").GetBoolean(), "descriptor marks the base bitfield");
            }

            ProcessResult upper = await layout.RunUpperAsync(withLeaf: false);
            Verify.Equal(0, upper.ExitCode, "Upper module: " + upper.StdErr);
            string value = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            Verify.That(value.Contains(Compact("detail::BitfieldFieldOps<::probe::Value, &GeneratedType<::probe::Parent>::Get_flag, " +
                "&GeneratedType<::probe::Parent>::Set_flag, false>("), StringComparison.Ordinal), "inherited bitfield uses the base accessors");
            Verify.That(value.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "base generated header is included");
        });
    }
}
