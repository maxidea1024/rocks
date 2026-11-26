using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

/// <summary>
/// 리플렉션 헤더의 전처리 조건 블록 어휘 스캔이다(Docs/Design/BuildTargets 3.4절 BD6·BS2c, UHT가 헤더를 직접 읽어 조건 블록을 따라가는 것과
/// 같다). Clang JSON AST에는 조건 블록이 없으므로 원문을 읽는다. 주석·리터럴을 지우고(줄 번호 유지) 논리 줄(<c>\</c> 이음)로
/// <c>#if</c>·<c>#ifdef</c>·<c>#ifndef</c>·<c>#elif</c>·<c>#elifdef</c>·<c>#elifndef</c>·<c>#else</c>·<c>#endif</c> 중첩을 따른다.
/// <list type="bullet">
/// <item>리플렉션 매크로(<see cref="Macros"/>)가 조건 블록 안에 있으면 RMC2501이다. 예외는 깊이 1인 블록의 첫 가지 둘이다:
/// <c>#if ROCKS_WITH_EDITOR_DATA</c> 안 <c>ROCKS_PROPERTY</c>, <c>#if ROCKS_WITH_EDITOR</c> 안 <c>ROCKS_FUNCTION</c>(조건식은 공백을 정규화한
/// 정확한 철자).</item>
/// <item><see cref="InEditorDataBlock"/>은 필드 줄이 허용된 에디터 데이터 블록 안인지 답한다(양방향 일치 RMC2502가 쓴다).</item>
/// </list>
/// 사용 예: <c>ConditionalScan scan = ConditionalScan.Scan(options.Inputs);</c>
/// </summary>
internal sealed class ConditionalScan
{
    /// <summary>조건 블록 안에 둘 수 없는 리플렉션 매크로다(예외 둘은 위).</summary>
    public static readonly IReadOnlyList<string> Macros =
    [
        "ROCKS_CLASS", "ROCKS_STRUCT", "ROCKS_ENUM", "ROCKS_ENUMERATOR", "ROCKS_PROPERTY", "ROCKS_FUNCTION", "ROCKS_NATIVE", "ROCKS_GENERATED_BODY"
    ];

    /// <summary>에디터 전용 필드 블록의 조건 철자다.</summary>
    public const string EditorDataCondition = "ROCKS_WITH_EDITOR_DATA";

    /// <summary>에디터 전용 함수 블록의 조건 철자다.</summary>
    public const string EditorCondition = "ROCKS_WITH_EDITOR";

    private static readonly Regex Directive = new(@"^\s*#\s*([A-Za-z_]+)\b(.*)$", RegexOptions.CultureInvariant | RegexOptions.Singleline);

    private static readonly Regex MacroUse = new(@"\b(" + string.Join("|", Macros) + @")\b", RegexOptions.CultureInvariant);

    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    // 파일(정규 경로) → 허용된 에디터 데이터 블록 첫 가지의 줄 범위(여는 #if 줄부터 닫는 #else·#elif·#endif 줄까지)다.
    private readonly Dictionary<string, List<(int First, int Last)>> editorDataBlocks = new(PathComparer);

    // 허용된 에디터 블록(#if ROCKS_WITH_EDITOR의 첫 가지)의 줄 범위다(EditorOnly 함수 일치 — Docs/Design/FunctionReflection.ko.md FD14).
    private readonly Dictionary<string, List<(int First, int Last)>> editorBlocks = new(PathComparer);

    private enum BlockKind { EditorData, Editor, Other }

    private sealed class Block(BlockKind kind, int line)
    {
        public BlockKind Kind { get; } = kind;

        public int Line { get; } = line;

        public bool FirstBranch { get; set; } = true;
    }

    /// <summary>입력 헤더를 모두 훑는다. 블록 안 리플렉션 선언이 있으면 첫 위반에서 RMC2501을 던진다.</summary>
    /// <exception cref="MetadataException">RMC2501.</exception>
    public static ConditionalScan Scan(IReadOnlyList<ModuleInput> inputs)
    {
        var scan = new ConditionalScan();
        foreach (ModuleInput input in inputs)
        {
            scan.ScanFile(input.Path, File.ReadAllText(input.Path));
        }

        return scan;
    }

    /// <summary>텍스트 하나를 훑는다(시험용 — path는 진단 위치다).</summary>
    /// <exception cref="MetadataException">RMC2501.</exception>
    public static ConditionalScan ScanText(string path, string text)
    {
        var scan = new ConditionalScan();
        scan.ScanFile(path, text);
        return scan;
    }

    /// <summary>file의 line(1부터)이 허용된 <c>#if ROCKS_WITH_EDITOR_DATA</c> 블록의 첫 가지 안인가다.</summary>
    public bool InEditorDataBlock(string file, int line) =>
        editorDataBlocks.TryGetValue(Key(file), out List<(int First, int Last)>? blocks) && blocks.Any(block => line > block.First && line < block.Last);

    // 함수 줄이 허용된 에디터 블록(#if ROCKS_WITH_EDITOR) 안인지다(EditorOnly 함수 일치).
    public bool InEditorBlock(string file, int line) =>
        editorBlocks.TryGetValue(Key(file), out List<(int First, int Last)>? blocks) && blocks.Any(block => line > block.First && line < block.Last);

    private static string Key(string path) => File.Exists(path) ? CanonicalPath.Resolve(path) : Path.GetFullPath(path);

    private void ScanFile(string path, string text)
    {
        string[] lines = CppSourceText.WithoutCommentsAndLiterals(text).Split('\n');
        var stack = new List<Block>();
        var blocks = new List<(int First, int Last)>();
        var editor = new List<(int First, int Last)>();
        for (int index = 0; index < lines.Length; ++index)
        {
            int lineNumber = index + 1;
            string line = lines[index].TrimEnd('\r');
            Match directive = Directive.Match(line);
            if (directive.Success)
            {
                // 논리 줄: 역슬래시로 이어진 다음 줄을 붙인다(조건식이 여러 줄일 수 있다).
                string logical = line;
                while (logical.TrimEnd().EndsWith('\\') && index + 1 < lines.Length)
                {
                    logical = logical.TrimEnd()[..^1] + " " + lines[++index].TrimEnd('\r');
                }

                directive = Directive.Match(logical);
                string name = directive.Groups[1].Value;
                string condition = Regex.Replace(directive.Groups[2].Value, @"\s+", " ").Trim();
                switch (name)
                {
                    case "if":
                        stack.Add(new(condition switch
                        {
                            EditorDataCondition => BlockKind.EditorData,
                            EditorCondition => BlockKind.Editor,
                            _ => BlockKind.Other
                        }, lineNumber));
                        break;
                    case "ifdef" or "ifndef":
                        stack.Add(new(BlockKind.Other, lineNumber));
                        break;
                    case "elif" or "elifdef" or "elifndef" or "else" when stack.Count != 0:
                        Close(stack[^1], lineNumber, blocks, editor);
                        stack[^1].FirstBranch = false;
                        break;
                    case "endif" when stack.Count != 0:
                        Close(stack[^1], lineNumber, blocks, editor);
                        stack.RemoveAt(stack.Count - 1);
                        break;
                }

                continue;
            }

            if (stack.Count == 0)
            {
                continue;
            }

            foreach (Match use in MacroUse.Matches(line))
            {
                string macro = use.Groups[1].Value;
                Block top = stack[^1];
                bool allowed = stack.Count == 1 && top.FirstBranch &&
                    ((macro == "ROCKS_PROPERTY" && top.Kind == BlockKind.EditorData) || (macro == "ROCKS_FUNCTION" && top.Kind == BlockKind.Editor));
                if (!allowed)
                {
                    throw new MetadataException($"{macro} is inside a preprocessor conditional block (opened at line {top.Line})", "RMC2501",
                        "Declare reflected types, enumerators and native members outside conditional blocks; only ROCKS_PROPERTY(EditorOnly, ...) may " +
                        "sit directly in '#if ROCKS_WITH_EDITOR_DATA' and ROCKS_FUNCTION(EditorOnly, ...) directly in '#if ROCKS_WITH_EDITOR' " +
                        "(Docs/Design/BuildTargets section 3.4).").At(new SourceSite(path, lineNumber));
                }
            }
        }

        if (blocks.Count != 0)
        {
            editorDataBlocks[Key(path)] = blocks;
        }

        if (editor.Count != 0)
        {
            editorBlocks[Key(path)] = editor;
        }
    }

    // 블록의 첫 가지가 끝난다. 허용된 에디터 데이터 블록·에디터 블록이면 그 줄 범위를 남긴다.
    private static void Close(Block block, int lineNumber, List<(int First, int Last)> blocks, List<(int First, int Last)> editor)
    {
        if (block.FirstBranch && block.Kind == BlockKind.Editor)
        {
            editor.Add((block.Line, lineNumber));
        }

        if (block.FirstBranch && block.Kind == BlockKind.EditorData)
        {
            blocks.Add((block.Line, lineNumber));
        }
    }
}
