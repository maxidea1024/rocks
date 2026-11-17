using Rocks.Build.Rules;

// trace 분석(UE TraceAnalysis·TraceServices 대응, Docs/Design/TraceSystem.ko.md 3·10절 TR4): 스트림 읽기·스레드 병합·구간·task·
// 카운터·표지 모델·결정적 JSON report. 형식의 단일 출처는 TraceCapture의 TraceFormat.h다. 도구(Commandlet)·시험만 링크하고 게임
// 실행 파일에는 들어가지 않는다(Developer 모듈).
public sealed class TraceAnalysis : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "TraceAnalysis";

    protected override string[] RequiredModules => ["Core", "Compression", "TraceCapture"];
}
