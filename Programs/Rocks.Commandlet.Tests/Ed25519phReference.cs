using System.Numerics;
using System.Security.Cryptography;

// RFC 8032 5.1 Ed25519ph의 독립 참조 구현이다(순수 BigInteger — 엔진·libsodium 코드를 쓰지 않는다). 콘텐츠 서명 봉투의
// golden byte를 다시 만들고(Docs/Design/ContentSigning.ko.md 7절 7), content-sign이 쓴 봉투의 서명을 엔진과 무관하게
// 확인한다. 느린 교과서 구현이며 시험 전용이다(상수 시간이 아니다).
internal static class Ed25519phReference
{
    private static readonly BigInteger P = BigInteger.Pow(2, 255) - 19;
    private static readonly BigInteger L = BigInteger.Pow(2, 252) + BigInteger.Parse("27742317777372353535851937790883648493");
    private static readonly BigInteger D = Mod(-121665 * Inverse(121666));
    private static readonly BigInteger I = BigInteger.ModPow(2, (P - 1) / 4, P);
    private static readonly (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) B = BasePoint();

    // 콘텐츠 서명 메시지의 도메인 분리 label이다(끝 NUL 포함 — ContentSigning 3절).
    public static readonly byte[] ContentLabel = "rocks.content-signature.v1\0"u8.ToArray();

    public static byte[] PublicKey(byte[] seed)
    {
        (BigInteger a, _) = Expand(seed);
        return Encode(Multiply(a, B));
    }

    public static byte[] Sign(byte[] seed, byte[] message)
    {
        (BigInteger a, byte[] prefix) = Expand(seed);
        byte[] publicKey = Encode(Multiply(a, B));
        byte[] hash = SHA512.HashData(message);
        byte[] dom = Dom2();
        BigInteger r = Mod(FromLittleEndian(SHA512.HashData([.. dom, .. prefix, .. hash])), L);
        byte[] encodedR = Encode(Multiply(r, B));
        BigInteger k = Mod(FromLittleEndian(SHA512.HashData([.. dom, .. encodedR, .. publicKey, .. hash])), L);
        BigInteger s = Mod(r + k * a, L);
        return [.. encodedR, .. ToLittleEndian(s, 32)];
    }

    // 서명을 확인한다(공개키 점 복원 실패·S 범위 밖은 거짓). [S]B = R + [k]A를 비교한다.
    public static bool Verify(byte[] publicKey, byte[] message, byte[] signature)
    {
        if (signature.Length != 64 || publicKey.Length != 32)
        {
            return false;
        }

        var a = Decode(publicKey);
        var r = Decode(signature[..32]);
        if (a is null || r is null)
        {
            return false;
        }

        BigInteger s = FromLittleEndian(signature[32..]);
        if (s >= L)
        {
            return false;
        }

        byte[] hash = SHA512.HashData(message);
        BigInteger k = Mod(FromLittleEndian(SHA512.HashData([.. Dom2(), .. signature[..32], .. publicKey, .. hash])), L);
        return Encode(Multiply(s, B)).AsSpan().SequenceEqual(Encode(Add(r.Value, Multiply(k, a.Value))));
    }

    // 봉투 v1 = 헤더(72) ‖ 서명(64) ‖ 매니페스트이고 서명 메시지는 label ‖ 헤더 ‖ 매니페스트다(ContentSigning 3절).
    public static byte[] Envelope(byte[] seed, byte[] keyId, ulong version, byte[] scope, byte[] manifest)
    {
        byte[] header = new byte[72];
        "RSIG"u8.CopyTo(header);
        header[4] = 1;
        header[5] = 2;
        keyId.CopyTo(header, 8);
        BitConverter.TryWriteBytes(header.AsSpan(24), version);
        scope.CopyTo(header, 32);
        BitConverter.TryWriteBytes(header.AsSpan(64), (ulong)manifest.Length);
        byte[] signature = Sign(seed, [.. ContentLabel, .. header, .. manifest]);
        return [.. header, .. signature, .. manifest];
    }

    // 봉투의 서명을 publicKey로 확인한다(헤더 판정은 하지 않는다 — 서명 byte만 독립 확인).
    public static bool VerifyEnvelope(byte[] publicKey, byte[] envelope)
    {
        if (envelope.Length < 136)
        {
            return false;
        }

        byte[] message = [.. ContentLabel, .. envelope[..72], .. envelope[136..]];
        return Verify(publicKey, message, envelope[72..136]);
    }

    private static byte[] Dom2()
    {
        // "SigEd25519 no Ed25519 collisions" ‖ phflag 1 ‖ context 길이 0.
        return [.. "SigEd25519 no Ed25519 collisions"u8, 1, 0];
    }

    private static (BigInteger, byte[]) Expand(byte[] seed)
    {
        byte[] h = SHA512.HashData(seed);
        BigInteger a = FromLittleEndian(h[..32]);
        a &= (BigInteger.One << 254) - 8;
        a |= BigInteger.One << 254;
        return (a, h[32..]);
    }

    private static BigInteger Mod(BigInteger value, BigInteger modulus)
    {
        BigInteger result = value % modulus;
        return result.Sign < 0 ? result + modulus : result;
    }

    private static BigInteger Mod(BigInteger value)
    {
        return Mod(value, P);
    }

    private static BigInteger Inverse(BigInteger value)
    {
        return BigInteger.ModPow(Mod(value), P - 2, P);
    }

    private static BigInteger? RecoverX(BigInteger y, int sign)
    {
        BigInteger xx = Mod((y * y - 1) * Inverse(D * y * y + 1));
        BigInteger x = BigInteger.ModPow(xx, (P + 3) / 8, P);
        if (Mod(x * x - xx) != 0)
        {
            x = Mod(x * I);
        }

        if (Mod(x * x - xx) != 0)
        {
            return null;
        }

        if ((int)(x & 1) != sign)
        {
            x = P - x;
        }

        return x;
    }

    private static (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) BasePoint()
    {
        BigInteger y = Mod(4 * Inverse(5));
        BigInteger x = RecoverX(y, 0)!.Value;
        return (x, y, 1, Mod(x * y));
    }

    private static (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) Add(
        (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) p,
        (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) q)
    {
        BigInteger a = Mod((p.Y - p.X) * (q.Y - q.X));
        BigInteger b = Mod((p.Y + p.X) * (q.Y + q.X));
        BigInteger c = Mod(p.T * 2 * D * q.T);
        BigInteger d = Mod(p.Z * 2 * q.Z);
        BigInteger e = b - a;
        BigInteger f = d - c;
        BigInteger g = d + c;
        BigInteger h = b + a;
        return (Mod(e * f), Mod(g * h), Mod(f * g), Mod(e * h));
    }

    private static (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) Multiply(
        BigInteger scalar, (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) point)
    {
        (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) result = (0, 1, 1, 0);
        while (scalar > 0)
        {
            if (!scalar.IsEven)
            {
                result = Add(result, point);
            }

            point = Add(point, point);
            scalar >>= 1;
        }

        return result;
    }

    private static byte[] Encode((BigInteger X, BigInteger Y, BigInteger Z, BigInteger T) point)
    {
        BigInteger inverse = Inverse(point.Z);
        BigInteger x = Mod(point.X * inverse);
        BigInteger y = Mod(point.Y * inverse);
        return ToLittleEndian(y | ((x & 1) << 255), 32);
    }

    private static (BigInteger X, BigInteger Y, BigInteger Z, BigInteger T)? Decode(byte[] encoded)
    {
        BigInteger value = FromLittleEndian(encoded);
        BigInteger y = value & ((BigInteger.One << 255) - 1);
        int sign = (int)(value >> 255);
        if (y >= P)
        {
            return null;
        }

        BigInteger? x = RecoverX(y, sign);
        if (x is null)
        {
            return null;
        }

        return (x.Value, y, 1, Mod(x.Value * y));
    }

    private static BigInteger FromLittleEndian(ReadOnlySpan<byte> bytes)
    {
        return new BigInteger(bytes, isUnsigned: true, isBigEndian: false);
    }

    private static byte[] ToLittleEndian(BigInteger value, int length)
    {
        byte[] result = new byte[length];
        value.TryWriteBytes(result, out _, isUnsigned: true, isBigEndian: false);
        return result;
    }
}
