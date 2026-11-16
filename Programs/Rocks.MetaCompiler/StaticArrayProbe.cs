using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 정적 배열 PROPERTY(T name[N] — 리플렉션 B9g, Docs/Design/ReflectionExtensions.ko.md 8절)의 원소 형식을 정규 철자로 푸는 탐침이다.
// Clang JSON 덤프는 배열 형식을 쓰인 그대로 출력한다(배열은 sugar가 아니라 desugaredQualType이 없고 원소는 `int16`·
// `ObjectRef<Owner>`처럼 별칭·한정 없는 이름이다). 그래서 Binds 해석(CompilerDriver.ResolveBindsAsync)·NATIVE 탐침
// (NativeMemberProbe)과 같이 첫 실행과 같은 번역 단위 끝에 필드마다
// `static constexpr ::rocks_meta_static::RocksStaticShape<decltype(::Owner::field)>::rocksStaticCode rocksStatic_N{};`를 붙여 Clang을
// 다시 실행하고(-ast-dump-filter로 그 변수만 덤프) 변수 형식의 desugar 철자 `rocks_meta_static::RocksStaticCode<원소, N>`을 읽는다.
// 템플릿 인자는 정규 형식으로 출력되므로 원소 철자가 배열·Map 원소와 같은 정규 철자(`short`, `rocks::ObjectRef<ns::Owner>`)가 된다.
// 탐침 변수는 ROCKS_GENERATED_BODY가 friend로 두는 GeneratedType의 특수화 안에 두어 private 멤버에도 닿는다. 다차원 배열은 원소가
// 다시 배열(`int[2]`)로 나오고 SchemaValidator가 RMC2307로 거부한다. C 배열 ROCKS_NATIVE 멤버도 같은 실행에서 모든 차원을 벗긴 원소의
// 정규 철자를 얻어(NativeMemberModel.ElementSpelling) 생성 코드가 원소 struct의 형질(RMC2205·2206)을 한 번 더 확인하게 한다.
internal static class StaticArrayProbe
{
    private const string Templates = """
        #include <Meta/GeneratedType.h>
        namespace rocks_meta_static {
        template <class RocksStaticE, decltype(sizeof(0)) RocksStaticN>
        struct RocksStaticCode {};
        template <class RocksStaticT>
        struct RocksStaticShape {
            using rocksStaticCode = RocksStaticCode<void, 0>;
        };
        template <class RocksStaticE, decltype(sizeof(0)) RocksStaticN>
        struct RocksStaticShape<RocksStaticE[RocksStaticN]> {
            using rocksStaticCode = RocksStaticCode<RocksStaticE, RocksStaticN>;
        };
        template <class RocksStaticT>
        struct RocksStaticBase {
            using rocksStaticType = RocksStaticT;
        };
        template <class RocksStaticE, decltype(sizeof(0)) RocksStaticN>
        struct RocksStaticBase<RocksStaticE[RocksStaticN]> : RocksStaticBase<RocksStaticE> {};
        template <class RocksStaticE>
        struct RocksStaticBase<RocksStaticE[]> : RocksStaticBase<RocksStaticE> {};
        struct RocksStaticTag;
        }

        """;

    private const string ProbePrefix = "rocksStatic_";

    // 탐침 변수의 desugar된 형식이다: const RocksStaticCode<원소의 정규 철자, N>(N은 접미사 U·L이 붙을 수 있다).
    private static readonly Regex CodeType = new(@"^(?:const )?rocks_meta_static::RocksStaticCode<(?<element>.+), (?<count>\d+)[uUlL]*>$",
        RegexOptions.CultureInvariant | RegexOptions.Singleline, TimeSpan.FromSeconds(1));

    // 이 모듈 PROPERTY 가운데 첫 실행 철자가 C 배열인 필드의 정규 철자(`원소[N]`)를 FieldModel.CanonicalSpelling에, C 배열 ROCKS_NATIVE
    // 멤버의 모든 차원을 벗긴 원소 정규 철자를 NativeMemberModel.ElementSpelling에 채운다(생성 코드의 2차 확인이 원소 struct 형질을 보게 —
    // `RefStruct cache[2]`). 그런 필드·멤버가 없으면 Clang을 실행하지 않는다. run은 첫 실행 번역 단위 끝에 텍스트를 붙여 경고를 끄고
    // 탐침 변수만 덤프하는 Clang 실행이다. 실행이 실패하면 RMC3000, 변수를 읽지 못하면 RMC2001이다.
    public static async Task ResolveAsync(Dictionary<string, ClassModel> declared, Func<string, Task<ProcessResult>> run)
    {
        List<FieldModel> fields = declared.Values.Where(model => !model.IsEnum).SelectMany(model => model.OwnFields)
            .Where(field => StaticArrayRules.Split(field.NativeSpelling) is not null).ToList();
        List<(string Owner, NativeMemberModel Native)> natives = declared.Values.Where(model => !model.IsEnum)
            .SelectMany(model => model.NativeMembers.Select(native => (model.Name, native)))
            .Where(entry => StaticArrayRules.Split(ClangAstParser.CleanType(entry.native.Spelling)) is not null).ToList();
        if (fields.Count == 0 && natives.Count == 0)
        {
            return;
        }

        StringBuilder text = new(Templates);
        text.Append("template <>\nstruct rocks::GeneratedType<rocks_meta_static::RocksStaticTag> {\n");
        for (int index = 0; index < fields.Count; ++index)
        {
            text.Append(CultureInfo.InvariantCulture,
                $"static constexpr ::rocks_meta_static::RocksStaticShape<decltype(::{fields[index].Owner}::{fields[index].Name})>::rocksStaticCode {ProbePrefix}{index}{{}};\n");
        }

        // NATIVE 멤버는 필드 뒤 번호를 쓴다(원소는 모든 차원을 벗긴 형식, N 자리는 0).
        for (int index = 0; index < natives.Count; ++index)
        {
            text.Append(CultureInfo.InvariantCulture,
                $"static constexpr ::rocks_meta_static::RocksStaticCode<typename ::rocks_meta_static::RocksStaticBase<decltype(::{natives[index].Owner}::{natives[index].Native.Name})>::rocksStaticType, 0> {ProbePrefix}{fields.Count + index}{{}};\n");
        }

        text.Append("};\n");
        ProcessResult result = await run(text.ToString());
        if (result.ExitCode != 0)
        {
            (string symbol, SourceSite site) = fields.Count != 0 ? ($"{fields[0].Owner}::{fields[0].Name}", fields[0].Site)
                : ($"{natives[0].Owner}::{natives[0].Native.Name}", natives[0].Native.Site);
            throw new MetadataException($"{symbol}: cannot resolve static array element types:\n" + result.StdErr, "RMC3000",
                "Make the reflected headers compile on their own; static array element types are resolved by Clang.").At(site);
        }

        Utf8JsonReader reader = new(Encoding.UTF8.GetBytes(result.StdOut), new JsonReaderOptions { AllowMultipleValues = true, MaxDepth = 2048 });
        while (reader.Read())
        {
            using JsonDocument probe = JsonDocument.ParseValue(ref reader);
            JsonElement node = probe.RootElement;
            string kind = node.TryGetProperty("kind", out JsonElement kindValue) ? kindValue.GetString() ?? "" : "";
            string name = node.TryGetProperty("name", out JsonElement nameValue) ? nameValue.GetString() ?? "" : "";
            if (kind != "VarDecl" || !name.StartsWith(ProbePrefix, StringComparison.Ordinal) ||
                !int.TryParse(name.AsSpan(ProbePrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out int index) ||
                index < 0 || index >= fields.Count + natives.Count)
            {
                continue;
            }

            Match match = CodeType.Match(ClangAstParser.TypeSpelling(node));
            if (!match.Success || match.Groups["element"].Value == "void")
            {
                continue;
            }

            if (index < fields.Count)
            {
                fields[index].CanonicalSpelling = $"{match.Groups["element"].Value}[{match.Groups["count"].Value}]";
            }
            else
            {
                natives[index - fields.Count].Native.ElementSpelling = match.Groups["element"].Value;
            }
        }

        FieldModel? unresolved = fields.FirstOrDefault(field => field.CanonicalSpelling is null);
        if (unresolved is not null)
        {
            throw new MetadataException($"{unresolved.Owner}::{unresolved.Name}: Clang did not report the static array element type of '{unresolved.NativeSpelling}'", "RMC2001",
                "Use the supported Clang/toolchain; add a reviewed parser case before accepting a new AST shape.").At(unresolved.Site);
        }

        (string Owner, NativeMemberModel Native) missing = natives.FirstOrDefault(entry => entry.Native.ElementSpelling is null);
        if (missing.Native is not null)
        {
            throw new MetadataException($"{missing.Owner}::{missing.Native.Name}: Clang did not report the array element type of '{missing.Native.Spelling}'", "RMC2001",
                "Use the supported Clang/toolchain; add a reviewed parser case before accepting a new AST shape.").At(missing.Native.Site);
        }
    }
}
