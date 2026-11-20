using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// ROCKS_NATIVE 멤버의 직접 타입이 Object 파생 클래스를 가리키는지(포인터·참조 — 여러 겹, cv·restrict·__unaligned·주소 공간·
// _Atomic·배열 포함)를 Clang이 판정하는 탐침이다(Docs/Design/ReflectionFoundations.ko.md 2.4절). 생성 헤더의 static_assert
// 형질은 대상 타입의 완전성에 기대지 않는다 — 그런 판정은 TU마다 값이 갈려 같은 특수화가 다른 정의를 갖는 ODR 위반이 된다
// (Object/NativeMemberTraits.h). 그래서 이 판정은 링크되지 않는 MetaCompiler 자신의 합성 번역 단위 하나에서만 한다.
//
// 방식은 Binds 해석(CompilerDriver.ResolveBindsAsync)과 같다: 첫 실행과 같은 번역 단위 끝에 멤버마다
// `static constexpr ::rocks_meta_probe::RocksProbeVerdict<decltype(::Owner::member)> rocksProbe_N{};`를 붙여 Clang을 다시
// 실행하고(-ast-dump-filter로 그 변수만 덤프한다) 변수 형식의 desugar 철자 `rocks_meta_probe::RocksProbeCode<판정, 대상>`을
// 읽는다. 별칭이 아니라 변수를 쓰는 이유: 별칭 선언의 JSON 덤프는 쓰인 형식 트리 전체(decltype이 가리키는 멤버 형식의 모든
// sugar 노드와 노드마다의 desugar 철자)를 내보내 깊은 중첩 템플릿 멤버에서 크기가 깊이의 제곱으로 커지지만(300겹 24MB,
// 800겹은 JSON 깊이 한도 초과), 변수 선언은 형식 철자 두 개만 낸다. 철자를 다시 쓰지 않고 decltype만 쓰므로 이름 가려짐·
// 특수화 선택·base 판정을 모두 Clang이 한다. 탐침 변수는 ROCKS_GENERATED_BODY가 friend로 두는 GeneratedType의 특수화 안에
// 두어 private·protected 멤버에도 접근 검사를 끄지 않고 닿는다. 탐침 텍스트의 이름은 모두 rocks_meta_probe·RocksProbe·
// rocksProbe 접두를 달아 리플렉션 헤더의 매크로(`#define Indirect 1` 등)와 겹치지 않게 한다. 값으로 담는 타입의 필드·템플릿
// 인자는 첫 실행 AST의 재귀 검사(ClangAstParser.StrongReferencePath)가 본다.
//
// 실패 격리: 대상 클래스의 완전성을 sizeof로 보므로 대상 템플릿의 인스턴스화가 hard error·치명적 오류(재귀 깊이)가 될 수
// 있고 그러면 번역 단위 전체가 실패한다. 그때는 진단의 instantiation note가 가리키는 `<stdin>` 줄을 탐침 변수 줄에 대응시켜
// 실패한 멤버를 찾고(다시 실행하지 않는다) 나머지 멤버만 다시 판정한다. 어느 멤버에도 대응하지 않는 오류가 있으면 판정
// 템플릿만(멤버 없이) 실행해 그것도 실패하면 멤버 탓이 아닌 전역 실패로 RMC3000을 한 번 내고, 아니면 멤버 묶음을 반으로
// 나눠 다시 판정한다. 실패한 멤버는 RMC2205 "cannot verify"(그 멤버의 Clang error 줄)다.
//
// 불완전한 대상(정의가 번역 단위에 없음)은 모듈 자신의 public 헤더(CompilerDriver.ModulePublicHeaders)를 더한 두 번째 탐침으로
// 다시 판정한다. 그 탐침도 같은 격리를 쓰고(실패한 멤버가 다른 멤버의 판정을 가리지 않는다), 어느 멤버에도 대응하지 않는
// 실패면 헤더를 하나씩(멤버 없이) 컴파일해 실패하는 헤더를 빼고 다시 실행한다. 그래도 불완전하거나 그 멤버의 탐침이
// 실패하면 허용한다(`World*` 같은 빌린 back-pointer — 탐침 때문에 모듈 빌드가 막히지 않게).
internal static class NativeMemberProbe
{
    // 판정 코드다(0은 해당 없음). Incomplete는 두 번째 탐침 뒤에도 남으면 허용한다.
    private const int ObjectTarget = 1;

    private const int Incomplete = 2;

    private const int StrongWrapper = 3;

    // 탐침 판정 템플릿이다. 한 겹씩 cv·restrict를 벗기고(RocksProbeUnqualified) 포인터·참조(RocksProbeLayer — 공용 형질
    // IsPointerV·RemovePointerT·IsReferenceV·RemoveReferenceT라 __unaligned·주소 공간 한정 포인터도 벗긴다. kind 1,
    // 그 뒤는 간접)·배열·_Atomic(RocksProbeShape, kind 2 — 간접 여부 유지)을 따라간다. 각 겹에서 강한 참조 래퍼면
    // (IsNativeStrongReference — 생성 static_assert와 같은 형질) 3, 간접으로 닿은 class면 완전성(sizeof SFINAE)을 보아 Object
    // 파생이면 1·아니면 0·불완전하면 2다. 값으로 담은 class·함수·멤버 포인터·스칼라는 0이다(함수 포인터의 반환·인자 형식은
    // 따라가지 않는다). _Atomic(T)의 원소 형식도 Base의 RemoveAtomicT로 얻어 탐침에는 내장을 직접 쓰지 않는다.
    private const string Templates = """
        #include <Base/TypeTraits.h>
        #include <Object/Object.h>
        #include <Object/NativeMemberTraits.h>
        namespace rocks_meta_probe {
        template <class RocksProbeT, class = void>
        struct RocksProbeComplete {
            static constexpr bool rocksProbeValue = false;
        };
        template <class RocksProbeT>
        struct RocksProbeComplete<RocksProbeT, decltype(void(sizeof(RocksProbeT)))> {
            static constexpr bool rocksProbeValue = true;
        };
        template <class RocksProbeT>
        using RocksProbeUnqualified = ::rocks::RemoveRestrictT<::rocks::RemoveCvT<RocksProbeT>>;
        template <class RocksProbeT, class = void>
        struct RocksProbeShape {
            static constexpr int rocksProbeKind = 0;
            using rocksProbeNext = void;
        };
        template <class RocksProbeT>
        struct RocksProbeShape<RocksProbeT, ::rocks::VoidT<::rocks::RemoveAtomicT<RocksProbeT>>> {
            static constexpr int rocksProbeKind = 2;
            using rocksProbeNext = ::rocks::RemoveAtomicT<RocksProbeT>;
        };
        template <class RocksProbeT>
        struct RocksProbeShape<RocksProbeT[], void> {
            static constexpr int rocksProbeKind = 2;
            using rocksProbeNext = RocksProbeT;
        };
        template <class RocksProbeT, decltype(sizeof(0)) RocksProbeN>
        struct RocksProbeShape<RocksProbeT[RocksProbeN], void> {
            static constexpr int rocksProbeKind = 2;
            using rocksProbeNext = RocksProbeT;
        };
        template <class RocksProbeT, bool = ::rocks::IsReferenceV<RocksProbeT>, bool = ::rocks::IsPointerV<RocksProbeT>>
        struct RocksProbeLayer : RocksProbeShape<RocksProbeT> {};
        template <class RocksProbeT, bool RocksProbePointer>
        struct RocksProbeLayer<RocksProbeT, true, RocksProbePointer> {
            static constexpr int rocksProbeKind = 1;
            using rocksProbeNext = ::rocks::RemoveReferenceT<RocksProbeT>;
        };
        template <class RocksProbeT>
        struct RocksProbeLayer<RocksProbeT, false, true> {
            static constexpr int rocksProbeKind = 1;
            using rocksProbeNext = ::rocks::RemovePointerT<RocksProbeT>;
        };
        template <class RocksProbeT, bool = RocksProbeComplete<RocksProbeT>::rocksProbeValue>
        struct RocksProbeClassLeaf {
            static constexpr int rocksProbeValue = 2;
            using rocksProbeType = RocksProbeT;
        };
        template <class RocksProbeT>
        struct RocksProbeClassLeaf<RocksProbeT, true> {
            static constexpr int rocksProbeValue = ::rocks::IsBaseOfV<::rocks::Object, RocksProbeT> ? 1 : 0;
            using rocksProbeType = RocksProbeT;
        };
        template <class RocksProbeT, bool RocksProbeIndirect, bool = ::rocks::IsClassV<RocksProbeT>>
        struct RocksProbeLeaf {
            static constexpr int rocksProbeValue = 0;
            using rocksProbeType = void;
        };
        template <class RocksProbeT>
        struct RocksProbeLeaf<RocksProbeT, true, true> : RocksProbeClassLeaf<RocksProbeT> {};
        template <class RocksProbeT, bool RocksProbeIndirect, int = RocksProbeLayer<RocksProbeT>::rocksProbeKind>
        struct RocksProbeWalk;
        template <class RocksProbeT, bool RocksProbeIndirect, bool = ::rocks::IsNativeStrongReference<RocksProbeT>>
        struct RocksProbeStep : RocksProbeWalk<RocksProbeT, RocksProbeIndirect> {};
        template <class RocksProbeT, bool RocksProbeIndirect>
        struct RocksProbeStep<RocksProbeT, RocksProbeIndirect, true> {
            static constexpr int rocksProbeValue = 3;
            using rocksProbeType = RocksProbeT;
        };
        template <class RocksProbeT, bool RocksProbeIndirect>
        struct RocksProbeWalk<RocksProbeT, RocksProbeIndirect, 0> : RocksProbeLeaf<RocksProbeT, RocksProbeIndirect> {};
        template <class RocksProbeT, bool RocksProbeIndirect>
        struct RocksProbeWalk<RocksProbeT, RocksProbeIndirect, 1>
            : RocksProbeStep<RocksProbeUnqualified<typename RocksProbeLayer<RocksProbeT>::rocksProbeNext>, true> {};
        template <class RocksProbeT, bool RocksProbeIndirect>
        struct RocksProbeWalk<RocksProbeT, RocksProbeIndirect, 2>
            : RocksProbeStep<RocksProbeUnqualified<typename RocksProbeLayer<RocksProbeT>::rocksProbeNext>, RocksProbeIndirect> {};
        template <int RocksProbeV, class RocksProbeT>
        struct RocksProbeCode {};
        template <class RocksProbeT, class RocksProbeS = RocksProbeStep<RocksProbeUnqualified<RocksProbeT>, false>>
        using RocksProbeVerdict = RocksProbeCode<RocksProbeS::rocksProbeValue, typename RocksProbeS::rocksProbeType>;
        struct RocksProbeTag;
        }

        """;

    private const string ProbePrefix = "rocksProbe_";

    // 판정 형판이다(레코드 닫힘이 중첩 포인터·참조 필드의 판정에 다시 쓴다 — Docs/Design/ClangVersionSupport.ko.md 4.4절).
    internal static string VerdictTemplates => Templates;

    // 판정 형식 철자다: (const) RocksProbeCode<판정, 대상의 정규 철자>(판정 0이면 대상은 void). 탐침 변수의 desugar 철자와
    // 정규 형식 탐침의 템플릿 인자가 같은 모양이다.
    private static readonly Regex VerdictType = new(@"^(?:const )?rocks_meta_probe::RocksProbeCode<(?<code>[0-3]), (?<target>.+)>$",
        RegexOptions.CultureInvariant | RegexOptions.Singleline, TimeSpan.FromSeconds(1));

    // Owner는 소유 타입의 C++ 정규 이름(앞의 :: 없음), Index는 탐침 변수 번호(실행이 달라도 같은 멤버는 같은 번호)다.
    private sealed record Member(int Index, string Owner, NativeMemberModel Native)
    {
        public string Symbol => Owner + "::" + Native.Name;
    }

    internal readonly record struct Verdict(int Code, string Target);

    // 판정 형식 철자 하나를 판정으로 읽는다(모양이 다르면 null).
    internal static Verdict? ParseVerdict(string spelling)
    {
        Match match = VerdictType.Match(spelling);
        return match.Success ? new Verdict(match.Groups["code"].Value[0] - '0', match.Groups["target"].Value) : null;
    }

    // headers(두 번째 탐침의 public 헤더) include 뒤에 판정 템플릿과 GeneratedType 특수화의 여는 줄을 둔다. 멤버별 탐침 변수가 질문
    // 줄이고 닫는 줄이 뒷부분이다(ClangProbe).
    private static string Prelude(IReadOnlyList<string> headers)
    {
        StringBuilder text = new();
        foreach (string header in headers)
        {
            text.Append($"#include \"{header.Replace('\\', '/')}\"\n");
        }

        return text.Append(Templates).Append("template <>\nstruct rocks::GeneratedType<rocks_meta_probe::RocksProbeTag> {\n").ToString();
    }

    private const string Epilogue = "};\n";

    private static List<ClangProbe.Question> Questions(IEnumerable<Member> members) => members
        .Select(member => new ClangProbe.Question(member.Index, string.Create(CultureInfo.InvariantCulture,
            $"static constexpr ::rocks_meta_probe::RocksProbeVerdict<decltype(::{member.Owner}::{member.Native.Name})> {ProbePrefix}{member.Index}{{}};")))
        .ToList();

    // types는 이 모듈 타입(생성 순서), baseLines는 합성 번역 단위의 줄 수, publicHeaders는 불완전한 대상이 있을 때만 부르는 모듈
    // public 헤더 목록이다(절대 경로). Object 파생 대상·강한 참조 래퍼가 있으면 첫 멤버를, 없고 판정하지 못한 멤버가 있으면 그
    // 첫 멤버를 그 선언 위치의 RMC2205로 던진다. 판정 템플릿 자체가 컴파일되지 않으면 RMC3000을 한 번 던진다.
    internal static async Task VerifyAsync(IEnumerable<ClassModel> types, int baseLines, Func<IReadOnlyList<string>> publicHeaders,
        Func<string, Task<ProcessResult>> run)
    {
        List<Member> members = [];
        foreach (ClassModel type in types)
        {
            // bitfield 멤버(정수·bool만 받는다 — 리플렉션 B9m)는 강한 참조를 담을 수 없으므로 탐침하지 않는다.
            foreach (NativeMemberModel native in type.NativeMembers.Where(native => !native.Bitfield))
            {
                members.Add(new(members.Count, type.NativeName, native));
            }
        }

        if (members.Count == 0)
        {
            return;
        }

        ClangProbe.Session<Verdict> session = new(baseLines, run, ReadVerdicts);
        ClangProbe.Outcome<Verdict> outcome = new();
        string prelude = Prelude([]);
        List<ClangProbe.Question> questions = Questions(members);
        ClangProbe.Run<Verdict> first = await session.RunAsync(prelude, questions, Epilogue);
        if (first.Answers is null && first.Unattributed)
        {
            ClangProbe.Run<Verdict> bare = await session.RunAsync(prelude, [], Epilogue);
            if (bare.Answers is null)
            {
                throw new MetadataException($"{members[0].Symbol}: Clang rejected the ROCKS_NATIVE member probe itself, before any member (Clang: {bare.Failure})", "RMC3000",
                    "The probe appended after the reflected headers failed with no member in it; check for a macro or declaration in the reflected headers that collides with the probe (rocks_meta_probe, RocksProbe*, rocksProbe*) and use the supported Clang toolchain.")
                    .At(members[0].Native.Site);
            }
        }

        await session.JudgeAsync(prelude, questions, Epilogue, outcome, first);
        List<Member> incomplete = members.Where(member => outcome.Answers.TryGetValue(member.Index, out Verdict verdict) && verdict.Code == Incomplete).ToList();
        if (incomplete.Count != 0)
        {
            if (await ClangProbe.WidenAsync(session, Prelude, Epilogue, Questions(incomplete), publicHeaders()) is ClangProbe.Outcome<Verdict> widened)
            {
                foreach ((int index, Verdict verdict) in widened.Answers)
                {
                    outcome.Answers[index] = verdict;
                }
            }
        }

        foreach (Member member in members)
        {
            if (outcome.Answers.TryGetValue(member.Index, out Verdict verdict) && verdict.Code is ObjectTarget or StrongWrapper)
            {
                string path = verdict.Code == ObjectTarget ? "raw pointer to Object-derived " + verdict.Target : verdict.Target;
                throw new MetadataException($"{member.Symbol}: ROCKS_NATIVE member holds a strong object reference ({path})", "RMC2205",
                    "Native members are not traced; store ObjectRef/RootRef/Object pointers in ROCKS_PROPERTY fields or use WeakObjectRef/ObjectHandle.").At(member.Native.Site);
            }
        }

        foreach (Member member in members)
        {
            if (outcome.Failures.TryGetValue(member.Index, out string? failure))
            {
                throw new MetadataException($"{member.Symbol}: cannot verify that ROCKS_NATIVE member holds no strong object reference (Clang: {failure})", "RMC2205",
                    "MetaCompiler completes the class the member points to (sizeof) to see whether it derives from rocks::Object; fix the Clang error so that instantiation compiles, or point to a type that does.")
                    .At(member.Native.Site);
            }
        }
    }

    private static Dictionary<int, Verdict>? ReadVerdicts(string output)
    {
        Dictionary<int, Verdict> verdicts = [];
        try
        {
            Utf8JsonReader reader = new(Encoding.UTF8.GetBytes(output), new JsonReaderOptions { AllowMultipleValues = true, MaxDepth = 2048 });
            while (reader.Read())
            {
                using JsonDocument probe = JsonDocument.ParseValue(ref reader);
                JsonElement node = probe.RootElement;
                string kind = node.TryGetProperty("kind", out JsonElement kindValue) ? kindValue.GetString() ?? "" : "";
                string name = node.TryGetProperty("name", out JsonElement nameValue) ? nameValue.GetString() ?? "" : "";
                if (kind != "VarDecl" || !name.StartsWith(ProbePrefix, StringComparison.Ordinal) ||
                    !int.TryParse(name.AsSpan(ProbePrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out int index))
                {
                    continue;
                }

                if (ParseVerdict(ClangAstParser.TypeSpelling(node)) is Verdict verdict)
                {
                    verdicts[index] = verdict;
                }
            }
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or KeyNotFoundException or RegexMatchTimeoutException)
        {
            return null;
        }

        return verdicts;
    }
}
