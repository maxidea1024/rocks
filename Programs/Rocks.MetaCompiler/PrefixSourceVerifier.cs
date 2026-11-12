namespace Rocks.MetaCompiler;

// 전처리된 클래스와 마커 사이 원문에 허용된 토큰만 있는지 보수적으로 확인한다.
internal static class PrefixSourceVerifier
{
    internal static void Verify(byte[] bytes, int start, int end)
    {
        int cursor = start;
        if (cursor < 0 || end <= cursor || end > bytes.Length)
            throw new MetadataException("Class marker location is unknown or ambiguous",
                hint: "Write ROCKS_CLASS directly in the source.");
        ReadOnlySpan<byte> remainder = bytes.AsSpan(cursor, end - cursor);
        string macro;
        if (remainder.StartsWith("ROCKS_CLASS"u8)) macro = "ROCKS_CLASS";
        else throw new MetadataException("ROCKS_CLASS marker is macro-generated or ambiguous",
            hint: "Write the class macro directly in the source; do not wrap it in another macro.");
        cursor += macro.Length;
        SkipTrivia();
        if (cursor >= end || bytes[cursor++] != (byte)'(') throw InvalidGap();
        int depth = 1;
        while (cursor < end && depth != 0)
        {
            byte value = bytes[cursor++];
            if (value == (byte)'(') ++depth;
            else if (value == (byte)')') --depth;
            else if (value is (byte)'"' or (byte)'\'')
            {
                // raw strings 및 line splice는 지원 문법에서 제외한다. 문자열 자체는 Clang이 검증한다.
                if (value == (byte)'"' && cursor >= 2 && bytes[cursor - 2] == (byte)'R') throw InvalidGap();
                bool closed = false;
                while (cursor < end)
                {
                    byte next = bytes[cursor++];
                    if (next == (byte)'\\')
                    {
                        if (cursor >= end || bytes[cursor] is (byte)'\n' or (byte)'\r') throw InvalidGap();
                        ++cursor;
                    }
                    else if (next == value) { closed = true; break; }
                    else if (next is (byte)'\n' or (byte)'\r') throw InvalidGap();
                }
                if (!closed) throw InvalidGap();
            }
            else if (value == (byte)'/' && cursor < end && bytes[cursor] is (byte)'/' or (byte)'*')
            {
                --cursor;
                SkipTrivia();
            }
            else if (value is (byte)'#' or (byte)'\\') throw InvalidGap();
        }
        if (depth != 0) throw InvalidGap();
        SkipTrivia();
        if (cursor != end) throw InvalidGap();
        ReadOnlySpan<byte> classSource = bytes.AsSpan(end);
        if (!(classSource.StartsWith("class"u8) || classSource.StartsWith("struct"u8))) throw InvalidGap();

        void SkipTrivia()
        {
            while (cursor < end)
            {
                byte value = bytes[cursor];
                if (value is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n' or (byte)'\f' or (byte)'\v') { ++cursor; continue; }
                if (value != (byte)'/' || cursor + 1 >= end) return;
                if (bytes[cursor + 1] == (byte)'/')
                {
                    cursor += 2;
                    while (cursor < end && bytes[cursor] is not ((byte)'\n' or (byte)'\r'))
                    {
                        RejectLineSplice();
                        ++cursor;
                    }
                    continue;
                }
                if (bytes[cursor + 1] != (byte)'*') return;
                cursor += 2;
                while (cursor + 1 < end && !(bytes[cursor] == (byte)'*' && bytes[cursor + 1] == (byte)'/'))
                {
                    RejectLineSplice();
                    ++cursor;
                }
                if (cursor + 1 >= end) throw InvalidGap();
                cursor += 2;
            }
        }
        void RejectLineSplice()
        {
            // Clang은 comment lexing 전에 역슬래시+개행을 제거한다. 이를 일반 주석으로
            // 지나치면 제한 lexer가 원래 전처리 경계를 잘못 읽을 수 있다.
            if (bytes[cursor] == (byte)'\\' && cursor + 1 < end &&
                bytes[cursor + 1] is (byte)'\n' or (byte)'\r')
                throw InvalidGap();
        }
        MetadataException InvalidGap() => new($"{macro} is not immediately before its class in source",
            hint: "Keep only whitespace and comments between the class macro and class; put both inside the same conditional block. Wrapper macros, raw strings and line splices are unsupported.");
    }
}
