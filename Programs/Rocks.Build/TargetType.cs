namespace Rocks.Build.Rules;

// 타깃 종류는 규칙(Rocks.Build.Abstractions의 TargetRules)과 기술자 해석(Descriptors — 타깃 종류에 따라 플러그인·모듈을 고른다,
// Docs/Design/BuildTargets.ko.md 3.9절)이 함께 쓴다. 기술자 해석이 이 어셈블리에 있으므로 enum도 여기 둔다. 이름공간은 규칙 작성자가
// 쓰는 Rocks.Build.Rules 그대로다.

/// <summary>
/// 타깃 종류다(UE 5.8 TargetType의 Game·Program 대응, Editor·Client·Server는 아직 없다).
/// <list type="bullet">
/// <item>Game: 출시하는 실행 파일과 그것이 링크하는 엔진 모듈 묶음이다. Release 구성에서 셰이더 시스템 Release 배제 정책
/// (Docs/Design/ShaderSystem.ko.md 9절)을 받는다: 모듈 도달 집합에 Developer 모듈이 없고 셰이더 컴파일러(d3dcompiler·dxcompiler)를
/// 링크하지 않으며, Windows에서는 OpenGLGraphics를 링크하지 않는다. 플러그인의 enabledByDefault가 적용된다.</item>
/// <item>Program: 개발 도구·시험·벤치마크 실행 파일이다(셰이더 워커, 시험 runner, Commandlet). UE처럼 Release에서도 Developer
/// 모듈을 쓸 수 있다(UE <c>bBuildDeveloperTools</c>는 Program 타깃이면 구성과 관계없이 켜진다). 플러그인은 타깃·프로젝트가 명시한
/// 것만 켜진다(enabledByDefault를 적용하지 않는다 — Docs/Design/BuildTargets.ko.md BD34).</item>
/// </list>
/// </summary>
public enum TargetType { Game, Program }
