# 앱 정보와 명령줄 — 15. C++과 C# 나란히 보기

[목차](README.ko.md)

## 15. C++과 C# 나란히 보기

같은 명령(`trace-report` — 예시라 지금 명령에 없는 `--format`·`--channel`을 더했다)을 양쪽으로 쓴다.

**C# (System.CommandLine 파싱 + `RocksCli` 출력)**

```csharp
Argument<FileInfo> input = new("input")
{
    Description = "Trace file to read",
};
Argument<FileInfo?> output = new("output")
{
    Description = "Report file to write; standard output when omitted",
    Arity = ArgumentArity.ZeroOrOne,
};
Option<ReportFormat> format = RocksCli.EnumOption("--format", "-f",
    ReportFormat.Text, "Report format");   // 도구 이름 정확 일치 파서와 기본값 표시를 붙인다
Option<string[]> channel = new("--channel")
{
    Description = "Only include these channels",
};

Command command = new("trace-report", "Convert a trace file into a text report.")
{
    input,
    output,
    format,
    channel,
};
RocksCli.AddExamples(command,
    "trace-report run.rtrace",
    "trace-report run.rtrace report.json --format json");
// 루트에서 RocksCli.Invoke(root, args)가 파서 설정·도움말·오류 출력·종료 코드를 맡는다(16.2절).
command.SetAction(result => TraceReport.Run(
    result.GetValue(input)!,
    result.GetValue(output),
    result.GetValue(format),
    result.GetValue(channel) ?? []));
```

**C++ (Command 옵션 struct)** — 13.2절 선언(약 25줄, enum 포함) + 등록 한 줄 + 실행 함수:

```cpp
commands.Add<TraceReportOptions>(&RunTraceReport);

Result<int32> RunTraceReport(CommandContext& context, const TraceReportOptions& options) noexcept {
    // options.input, options.output, options.format, options.channel을 바로 쓴다.
}
```

| 기준 | C# (System.CommandLine) | C++ (Rocks) |
|---|---|---|
| 명령 하나의 코드 양 | 선언 약 25줄 + action | 선언 약 25줄 + 등록 1줄 |
| 옵션을 더할 때 할 일 | `Option<T>` 하나를 만들고 명령에 넣고 action에서 꺼낸다 | 필드 하나와 `ROCKS_PROPERTY` 한 줄. `ids add`·`rocksids` 편집은 없다(identity 면제 — D30) |
| 실행 파일 하나에 한 번 드는 비용 | 패키지 참조(중앙 버전) | 헤더에 `ROCKS_STRUCT(Command(...))` 표지(빌드가 찾고 MetaCompiler가 그 모듈을 처리한다 — 규칙 수정 없음). `rocksids` 파일은 없다 |
| 옵션 한 줄로 파싱·검증·도움말 | 된다(`Option<T>` + `Description`) | 된다(필드 + `ROCKS_PROPERTY(Help=…)`) |
| 형식 변환 | 자동(`T`에서). 받는 표기는 `RocksCli` 사용자 파서가 C++과 같게 좁힌다 | 자동(필드 형식 + 생성 바인더) |
| 결과 꺼내기 | 옵션마다 `GetValue(option)` | struct 필드 그대로(`options.format`) |
| 필드 사이 검증 | `Validators.Add`(문구는 14.4절 규칙) | `ValidateCommand` hook |
| 예시 절 | `RocksCli.AddExamples`(System.CommandLine에 없음) | `Command(Examples=…)` |
| 빌드 시간 | 없음 | 그 모듈의 MetaCompiler 실행(생성 TU가 바뀔 때만 다시 컴파일) |

두 쪽의 `--help` 출력은 14.5절 "명령 상세" 예와 byte 단위로 같아야 한다. 둘 다 출력 규약(14.4·14.5절)을 각자 렌더한 결과다. 프로그램 이름만 다르고, 시험은 같은 이름을 준다. 17절 golden 시험이 이를 확인한다.
