using System.Numerics;
using System.Text;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 결정적 수학 모듈의 8비트 sRGB 표(<see cref="OutputPath"/>)를 정확한 유리수·정수 비교로 만든다(rule id
/// <c>math-tables</c>). 지수 2.4 = 12/5이므로 후보 float f에 대해 f^5와 q^12를 BigInteger로 비교해 반올림 방향을
/// 정한다(부동소수점 근사에 기대지 않는다). 설계: Docs/Design/CoreDataFoundations.ko.md 2.2절.
/// <list type="bullet">
/// <item>SrgbDecodeBits[k] = IEC 61966-2-1 decode(k/255)를 가장 가까운 float로 반올림한 값(동점은 짝수).</item>
/// <item>SrgbEncodeThresholdBits[k] = decode((k + 0.5)/255) 이상인 가장 작은 float. 선형 값 x의 8비트 부호화는
/// x 이하인 경계의 개수다(부호화 공간 중간점의 round-half-up과 같다).</item>
/// </list>
/// </summary>
public static class MathTables
{
    public const string OutputPath = "Core/Source/Runtime/DeterministicMath/Private/SrgbTables.generated.h";
    public const int GeneratorVersion = 1;

    /// <summary>생성한 헤더 텍스트다.</summary>
    public static string Generate()
    {
        var decode = new uint[256];
        for (int k = 0; k < 256; ++k)
        {
            if (k == 0)
            {
                decode[k] = 0;
            }
            else if (k <= 10)
            {
                decode[k] = RoundRational(5 * (BigInteger)k, 16473, ceiling: false); // k/255/12.92 = 5k/16473
            }
            else
            {
                decode[k] = PowerFloat(1000 * (BigInteger)k + 14025, 269025, ceiling: false); // ((k/255+0.055)/1.055)^2.4
            }
        }

        var thresholds = new uint[255];
        for (int k = 0; k < 255; ++k)
        {
            if (k <= 9)
            {
                thresholds[k] = RoundRational(5 * (2 * (BigInteger)k + 1), 32946, ceiling: true); // (k+0.5)/255/12.92
            }
            else
            {
                thresholds[k] = PowerFloat(2000 * (BigInteger)k + 29050, 538050, ceiling: true);
            }
        }

        for (int k = 1; k < 255; ++k)
        {
            if (thresholds[k] <= thresholds[k - 1])
            {
                throw new InvalidOperationException("sRGB thresholds must increase");
            }
        }

        var text = new StringBuilder();
        text.Append("// 생성 파일이다. 직접 고치지 않는다: Rocks.ToolTests math-tables --root . --write\n");
        text.Append($"// 생성기 버전 {GeneratorVersion}. 값은 float 비트 패턴이다(정확한 유리수·정수 비교로 반올림).\n");
        text.Append("#pragma once\n\n#include <Base/Types.h>\n\nnamespace rocks::deterministic::detail {\n\n");
        text.Append("// clang-format off\n");
        text.Append("// decode(k/255)를 가장 가까운 float로 반올림한 값.\n");
        Emit(text, "SrgbDecodeBits", decode);
        text.Append("// decode((k + 0.5)/255) 이상인 가장 작은 float. 선형 값 x의 부호화 = x 이하인 경계 수.\n");
        Emit(text, "SrgbEncodeThresholdBits", thresholds);
        text.Append("// clang-format on\n\n} // namespace rocks::deterministic::detail\n");
        return text.ToString();
    }

    // 이 규칙이 만드는 파일: sRGB 표와 정확도 기준값 표(MathReference).
    private static IEnumerable<(string Path, Func<string> Generate)> Outputs() =>
        [(OutputPath, Generate), (MathReference.OutputPath, MathReference.Generate)];

    /// <summary>저장소의 생성 파일이 지금 만든 결과와 같은지 본다.</summary>
    public static IReadOnlyList<CheckFailure> Check(string root)
    {
        var failures = new List<CheckFailure>();
        foreach (var (relative, generate) in Outputs())
        {
            string path = Path.Combine(Path.GetFullPath(root), relative);
            if (!Directory.Exists(Path.GetDirectoryName(path)!))
            {
                continue;
            }

            if (!File.Exists(path))
            {
                failures.Add(new(relative, 0, "math-tables", "generated math table is missing (run math-tables --write)"));
            }
            else if (File.ReadAllText(path, new UTF8Encoding(false)) != generate())
            {
                failures.Add(new(relative, 0, "math-tables", "generated math table is stale or edited (run math-tables --write)"));
            }
        }

        return failures;
    }

    /// <summary>생성 결과를 쓴다(write가 false면 차이만 본다). 바뀐 파일의 저장소 상대 경로를 돌려준다.</summary>
    public static IReadOnlyList<string> Run(string root, bool write)
    {
        var changed = new List<string>();
        foreach (var (relative, generate) in Outputs())
        {
            string expected = generate();
            string path = Path.Combine(Path.GetFullPath(root), relative);
            if (File.Exists(path) && File.ReadAllText(path, new UTF8Encoding(false)) == expected)
            {
                continue;
            }

            changed.Add(relative);
            if (!write)
            {
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string temporary = path + ".tmp";
            File.WriteAllText(temporary, expected, new UTF8Encoding(false));
            File.Move(temporary, path, overwrite: true);
        }

        return changed;
    }

    private static void Emit(StringBuilder text, string name, uint[] values)
    {
        text.Append($"inline constexpr uint32 {name}[] = {{\n");
        for (int index = 0; index < values.Length; index += 8)
        {
            text.Append("    ").Append(string.Join(", ", values.Skip(index).Take(8).Select(value => $"0x{value:X8}u"))).Append(",\n");
        }

        text.Append("};\n\n");
    }

    // 양의 정규 float = m·2^e(m ∈ [2^23, 2^24)).
    private readonly record struct Candidate(BigInteger M, int E)
    {
        public Candidate Next() => M + 1 == (BigInteger.One << 24) ? new(BigInteger.One << 23, E + 1) : new(M + 1, E);

        public Candidate Previous() => M == (BigInteger.One << 23) ? new((BigInteger.One << 24) - 1, E - 1) : new(M - 1, E);

        public uint Bits()
        {
            int exponent = E + 150;
            if (exponent <= 0 || exponent >= 255)
            {
                throw new InvalidOperationException("sRGB table value is not a normal float");
            }

            return ((uint)exponent << 23) | (uint)(M - (BigInteger.One << 23));
        }
    }

    // p/q(양수)를 float로: 가장 가까운 값(동점은 짝수) 또는 이상인 가장 작은 값.
    private static uint RoundRational(BigInteger p, BigInteger q, bool ceiling)
    {
        int e = (int)(BigInteger.Log(p, 2) - BigInteger.Log(q, 2)) - 23 - 2;
        while (true)
        {
            (BigInteger numerator, BigInteger denominator) = e >= 0 ? (p, q << e) : (p << -e, q);
            BigInteger m = BigInteger.DivRem(numerator, denominator, out BigInteger remainder);
            if (m >= (BigInteger.One << 24))
            {
                ++e;
                continue;
            }

            if (m < (BigInteger.One << 23))
            {
                --e;
                continue;
            }

            bool up = ceiling ? remainder > 0 : (remainder * 2 > denominator || (remainder * 2 == denominator && !m.IsEven));
            var candidate = new Candidate(m, e);
            return (up ? candidate.Next() : candidate).Bits();
        }
    }

    // (a/b)^(12/5)를 float로. 후보 값 c에 대해 c^5와 (a/b)^12를 정확히 비교한다.
    private static uint PowerFloat(BigInteger a, BigInteger b, bool ceiling)
    {
        BigInteger target = BigInteger.Pow(a, 12);
        BigInteger scale = BigInteger.Pow(b, 12);
        // (M·2^E)^5 와 target/scale의 비교: sign(M^5·2^(5E)·scale − target).
        int Compare(BigInteger m, int e)
        {
            BigInteger left = BigInteger.Pow(m, 5) * scale;
            BigInteger right = target;
            if (e >= 0)
            {
                left <<= 5 * e;
            }
            else
            {
                right <<= -5 * e;
            }

            return left.CompareTo(right);
        }

        float approximate = (float)Math.Pow((double)a / (double)b, 2.4);
        uint bits = BitConverter.SingleToUInt32Bits(approximate);
        var candidate = new Candidate(((BigInteger)(bits & 0x7FFFFF)) | (BigInteger.One << 23), (int)((bits >> 23) & 0xFF) - 150);
        if (ceiling)
        {
            // 이상인 가장 작은 float: c^5 ≥ target이고 이전 float는 아님.
            while (Compare(candidate.M, candidate.E) < 0)
            {
                candidate = candidate.Next();
            }

            while (true)
            {
                Candidate previous = candidate.Previous();
                if (Compare(previous.M, previous.E) < 0)
                {
                    break;
                }

                candidate = previous;
            }

            return candidate.Bits();
        }
        // 가장 가까운 float: 위·아래 중간점((2m ± 1)·2^(e−1))의 5제곱과 비교한다.
        while (true)
        {
            Candidate next = candidate.Next();
            // 위쪽 중간점 = (candidate + next)/2 = (2·M + 1)·2^(E−1)(같은 지수일 때). 지수가 바뀌면 next.M = 2^23·2이므로
            // 공통 지수로 맞춘다.
            (BigInteger upperM, int upperE) = next.E == candidate.E ? (2 * candidate.M + 1, candidate.E - 1) : (2 * candidate.M + 1, candidate.E - 1);
            // Compare < 0: 중간점^5 < 목표 → 목표가 위쪽 중간점보다 크다 → 다음 float가 더 가깝다.
            int upper = Compare(upperM, upperE);
            if (upper < 0 || (upper == 0 && !candidate.M.IsEven))
            {
                candidate = next;
                continue;
            }

            Candidate previous = candidate.Previous();
            (BigInteger lowerM, int lowerE) = previous.E == candidate.E
                ? (2 * candidate.M - 1, candidate.E - 1)
                : (4 * candidate.M - 1, candidate.E - 2); // 이전 float의 지수가 하나 작다: (M·2^E + M'·2^(E−1))/2
            // Compare > 0: 목표가 아래쪽 중간점보다 작다 → 이전 float가 더 가깝다.
            int lower = Compare(lowerM, lowerE);
            if (lower > 0 || (lower == 0 && !candidate.M.IsEven))
            {
                candidate = previous;
                continue;
            }

            return candidate.Bits();
        }
    }
}
