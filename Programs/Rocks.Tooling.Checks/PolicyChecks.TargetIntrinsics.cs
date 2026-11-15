using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 함수 단위로 CPU 기능을 켜는 속성 이름이다(target·target_clones·target_version, x86 cpu_specific·cpu_dispatch).
    private const string TargetAttributeName =
        "(?:__)?(?:target(?:_clones|_version)?|cpu_specific|cpu_dispatch)(?:__)?\\s*\\(";

    // 위 속성을 직접 거는 형태다. GNU 속성 목록(여러 속성·여러 줄), C++11 속성 목록의 속성 자리(`[[`·`,`·`::`·`using ns:` 뒤라
    // `[[assume(target(x))]]` 같은 인자 안의 호출은 아니다), __declspec, 파일 전체에 속성을 거는 #pragma clang attribute·
    // __pragma 형태(내용과 관계없이 Compiler.h 몫이다)와 GCC target pragma다. 주석·리터럴을 지우고 줄 이음을 공백으로 바꾼 전체
    // 텍스트에 맞춘다.
    private static readonly Regex TargetAttribute = Pattern(
        "__attribute__\\s*\\(\\((?:[^()]|\\([^()]*\\))*?\\b" + TargetAttributeName +
        "|\\[\\[(?:[^\\]]*?[,:])?\\s*" + TargetAttributeName +
        "|\\b__declspec\\s*\\(\\s*(?:cpu_specific|cpu_dispatch)\\s*\\(" +
        "|(?m:^)[ \\t]*#\\s*pragma\\s+(?:clang\\s+attribute\\b|GCC\\s+(?:target|push_options)\\b)" +
        "|\\b__pragma\\s*\\(\\s*(?:clang\\s+attribute\\b|GCC\\s+target\\b)");

    // 위 pragma의 _Pragma 연산자 형태다. pragma 내용이 리터럴이므로 주석만 지운 텍스트에 맞추고, _Pragma 토큰 자체가 리터럴 밖인지
    // 따로 확인한다.
    private static readonly Regex TargetPragmaOperator = Pattern(
        "\\b_Pragma\\s*\\(\\s*\"\\s*(?:clang\\s+attribute\\b|GCC\\s+(?:target|push_options)\\b)");

    // Compiler.h가 정의하는 함수 단위 target 매크로다. 모든 매크로는 어느 한 계열에 속해야 한다.
    private static readonly Regex TargetMacroName = Pattern("\\bROCKS_TARGET_[A-Za-z0-9_]+\\b");

    // 줄 이음(\ 뒤 줄바꿈)의 \다. 공백으로 바꾸면 길이와 줄바꿈 위치가 그대로라 offset·줄 번호가 원문과 같다.
    private static readonly Regex LineContinuation = Pattern("\\\\(?=\\r?\\n)");

    /// <summary>
    /// CPU 확장 명령 계열과 그 계열을 쓸 수 있는 디렉터리(저장소 상대, '/'로 끝남)다. 계열의 target 매크로와 intrinsics를 함께
    /// 묶는다. 범용 SIMD(SSE·AVX·NEON 산술)는 대상이 아니다.
    /// </summary>
    private sealed record IntrinsicFamily(string Name, Regex Pattern, string[] Directories);

    private static readonly IntrinsicFamily[] IntrinsicFamilies =
    [
        // AES 라운드(AES-NI·VAES·Key Locker, ARMv8 AESE·AESD·AESMC·AESIMC)와 carry-less 곱셈(PCLMULQDQ·VPCLMULQDQ, PMULL
        // 64 bit)이다. 쓰는 곳은 Crypto AES 경량 계층뿐이다(Docs/Design/Crypto.ko.md 5.3절). 그 builtin 형태도 같은 계열이다
        // (PlatformMath 헤더는 __builtin_*을 모두 허용받으므로 compiler-builtin만으로는 막히지 않는다).
        new("AES/carry-less multiply",
            Pattern("\\bROCKS_TARGET_(?:X64|ARM64)_AES\\b|\\b_mm(?:256|512)?_(?:aes[a-z0-9_]*|clmulepi64_[a-z0-9_]+)\\b" +
                    "|\\b_mm_(?:loadiwkey|encodekey(?:128|256)_u32)\\b|\\bvaes(?:e|d|mc|imc)q_u8\\b|\\bvmull(?:_high)?_p64\\b" +
                    "|\\b__builtin_ia32_(?:v?aes|v?pclmulqdq|loadiwkey|encodekey)[a-z0-9_]*\\b" +
                    "|\\b__builtin_neon_v(?:aes[a-z]*q_u8|mull_p64)\\b"),
            ["Core/Source/Runtime/Crypto/Private/Aes/"]),
        // SHA 계열 해시 명령(SHA-NI SHA-1·SHA-256·SHA-512, SM3·SM4, ARMv8 SHA1·SHA2·SHA512·SHA3·SM3·SM4)과 그 builtin 형태다.
        // 쓰는 곳은 Core SHA-256 하드웨어 경로뿐이다(Docs/Design/Rockpile.ko.md R0). 그 경로가 target 매크로를 Compiler.h에
        // 더하면 여기에 넣는다.
        new("SHA",
            Pattern("\\b_mm(?:256|512)?_(?:sha|sm3|sm4)[a-z0-9_]*\\b|\\bv(?:sha1|sha256|sha512|sm3|sm4)[a-z0-9_]*\\b" +
                    "|\\b(?:veor3q|vrax1q|vxarq|vbcaxq)_[a-z0-9]+\\b|\\b__builtin_ia32_(?:sha|vsha512|vsm3|vsm4)[a-z0-9_]*\\b" +
                    "|\\b__builtin_neon_v(?:sha1|sha256|sha512|sm3|sm4|eor3q|rax1q|xarq|bcaxq)[a-z0-9_]*\\b"),
            ["Core/Source/Runtime/Core/Private/Encoding/"]),
    ];

    /// <summary>
    /// 함수 단위 target 속성과 암호 확장 명령은 정해진 곳에서만 쓴다(Docs/Design/Crypto.ko.md 5.3절). 지원하지 않는 CPU에서
    /// 부르면 잘못된 명령 예외로 죽으므로, CPU 감지 뒤 고른 커널 표로만 부르는 디렉터리에 가둔다. 규칙 id는 둘이다.
    /// <list type="bullet">
    /// <item>target-attribute: target 속성(<c>__attribute__((target(...)))</c>·<c>[[gnu::target(...)]]</c>·target_clones·
    /// target_version·cpu_specific·cpu_dispatch)과 <c>#pragma clang attribute</c>(내용과 관계없이)·<c>#pragma GCC target</c>을
    /// Base/Compiler.h 밖에서 직접 쓴다. 허용 디렉터리 안에서도 대신 <c>ROCKS_TARGET_*</c> 매크로를 쓴다.</item>
    /// <item>target-intrinsics: 계열의 target 매크로·intrinsics를 그 계열의 디렉터리 밖에서 쓰거나(주석·리터럴 제외), 어느 계열에도
    /// 속하지 않은 <c>ROCKS_TARGET_*</c> 매크로를 정의·사용한다. AES·carry-less 곱셈과 <c>ROCKS_TARGET_X64_AES</c>·
    /// <c>ROCKS_TARGET_ARM64_AES</c>는 Crypto/Private/Aes/, SHA 계열은 Core/Private/Encoding/이다. 시험 소스도 같다(시험의 기준
    /// 구현은 스칼라다).</item>
    /// </list>
    /// 외부 라이브러리 원본은 <see cref="NativeSources"/>가 제외한다(libsodium의 target pragma는 manifest pragmaDirectives가
    /// 기록하고, zstd의 target 속성 매크로는 원본 SHA-256으로 고정된다). builtin 형태(<c>__builtin_ia32_aesenc128</c>)는
    /// <see cref="CompilerBuiltins"/>가 막는다. lexical 검사의 한계: 매크로로 속성·이름을 조립하는 형태(<c>#define A(x)
    /// __attribute__((x))</c>, 토큰 붙이기), inline asm 명령, NativeSources가 보지 않는 확장자(.inl·.hpp)는 잡지 않는다. 실수가
    /// 아니라 의도적 우회이므로 리뷰가 맡는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> TargetIntrinsics(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            string text = File.ReadAllText(path, StrictUtf8);
            string code = LineContinuation.Replace(WithoutCommentsAndLiterals(text), " ");
            bool definitionHeader = relative == TargetMacroDefinitionHeader;
            if (!definitionHeader)
            {
                string withLiterals = LineContinuation.Replace(WithoutComments(text), " ");
                SortedSet<int> attributeLines = [];
                foreach (Match match in TargetAttribute.Matches(code))
                {
                    attributeLines.Add(LineAt(code, match.Index + match.Value.Length - match.Value.TrimStart().Length));
                }

                foreach (Match match in TargetPragmaOperator.Matches(withLiterals))
                {
                    // 주석·리터럴 안의 _Pragma 문자열은 지운 텍스트에서 공백이다.
                    if (code[match.Index] == '_')
                    {
                        attributeLines.Add(LineAt(code, match.Index));
                    }
                }

                foreach (int line in attributeLines)
                {
                    failures.Add(new(relative, line, "target-attribute",
                        "function target attribute or #pragma clang attribute outside Base/Compiler.h (use a ROCKS_TARGET_* macro)"));
                }
            }

            foreach (Match match in TargetMacroName.Matches(code))
            {
                if (!IntrinsicFamilies.Any(family => family.Pattern.IsMatch(match.Value)))
                {
                    failures.Add(new(relative, LineAt(code, match.Index), "target-intrinsics",
                        $"{match.Value} is not assigned to an intrinsic family (add it to a family in PolicyChecks.TargetIntrinsics)"));
                }
            }

            // 정의 헤더는 target 속성으로 ROCKS_TARGET_*를 정의한다. intrinsics는 여기서도 계열 검사를 받는다.
            string[] lines = (definitionHeader ? TargetMacroName.Replace(code, " ") : code).Split('\n');
            foreach (IntrinsicFamily family in IntrinsicFamilies)
            {
                if (family.Directories.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)))
                {
                    continue;
                }

                for (int index = 0; index < lines.Length; ++index)
                {
                    if (family.Pattern.IsMatch(lines[index]))
                    {
                        failures.Add(new(relative, index + 1, "target-intrinsics",
                            $"{family.Name} target macro or intrinsic outside {string.Join(", ", family.Directories)}"));
                    }
                }
            }
        }

        return failures;
    }

    // text에서 index 위치의 1부터 센 줄 번호다.
    private static int LineAt(string text, int index) => 1 + text.AsSpan(0, index).Count('\n');
}
