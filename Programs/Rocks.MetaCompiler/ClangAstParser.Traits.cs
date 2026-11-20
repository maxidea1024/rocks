namespace Rocks.MetaCompiler;

// ClangAstParser 중 배열 필드 실패 진단(FindFieldAt)이 문구에 붙이는 쓴 철자 재귀다: Object 파생 여부(IsObjectDerived), 강한 객체
// 참조 경로(StrongReferencePath). 판정이 아니다 — NATIVE 멤버의 강한 참조·WorldOnly 형질 판정은 정규 형식 탐침의 레코드 닫힘
// (ClangAstParser.Canonical.cs)이 한다. 레코드 색인(IndexRecords)은 ClangAstParser.cs에 있다.
internal sealed partial class ClangAstParser
{
    private bool IsObjectDerived(string record, HashSet<string> visited)
    {
        if (record == "rocks::Object")
        {
            return true;
        }

        return visited.Add(record) && records.TryGetValue(record, out RecordInfo? info) &&
            info.Bases.Any(baseType => IsObjectDerived(CleanType(baseType), visited));
    }

    // NATIVE 멤버 타입이 강한 객체 참조를 담으면 그 경로를, 아니면 null을 돌려준다. 값으로 담는 필드·base·배열 원소·
    // 템플릿 인자와 패턴 필드를 따라간다. 포인터·참조는 대상이 Object 파생이거나 대상이 다시 강한 참조(T**,
    // ObjectRef<T>*)인지만 본다 — 빌린 포인터가 가리키는 비-Object 레코드의 필드는 그 소유자가 추적한다. Object 파생
    // 타입은 값이나 템플릿 인자로 닿아도 펼치지 않는다(객체는 Registry에 살고, 템플릿 인자로는 handle·borrow의 대상이다).
    private string? StrongReferencePath(string type, HashSet<string> visited, bool throughPointer = false)
    {
        string text = CleanType(type);
        if (text.Length == 0 || !visited.Add((throughPointer ? "*" : "") + text) || visited.Count > 512)
        {
            return null;
        }

        if (TopLevelContains(text, "(*)") || TopLevelContains(text, "(&)"))
        {
            return null; // 함수 포인터·참조
        }

        if (text.EndsWith(']'))
        {
            int bracket = text.LastIndexOf('[');
            return bracket > 0 ? StrongReferencePath(text[..bracket], visited, throughPointer) : null;
        }

        if (text.EndsWith('*') || text.EndsWith('&'))
        {
            string stripped = text.TrimEnd('&').TrimEnd();
            if (stripped.EndsWith('*'))
            {
                stripped = stripped[..^1];
            }

            string pointee = CleanType(stripped);
            if (IsObjectDerived(pointee, []))
            {
                return $"raw pointer to Object-derived {pointee}";
            }

            return StrongReferencePath(pointee, visited, throughPointer: true) is string inner ? $"{text} -> {inner}" : null;
        }

        if (SplitTemplate(text) is (string name, List<string> arguments))
        {
            string templateName = CleanType(name);
            if (StrongReferenceTemplates.Contains(templateName))
            {
                return templateName;
            }

            if (NonOwningTemplates.Contains(templateName) || throughPointer)
            {
                return null;
            }

            foreach (string argument in arguments)
            {
                if (StrongReferencePath(argument, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            text = templateName;
        }

        if (throughPointer || IsObjectDerived(text, []))
        {
            return null;
        }

        if (records.TryGetValue(text, out RecordInfo? info))
        {
            foreach (string member in info.Fields.Concat(info.Bases))
            {
                if (StrongReferencePath(member, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }
        }

        return null;
    }
}
