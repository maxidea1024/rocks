# Engine — 게임 엔진 계층

Core(기반 라이브러리) 위에 올라가는 게임 엔진 계층이다. World·Renderer·Gameplay·Audio·Physics·UI·Editor
모듈과 엔진 플러그인을 이 아래에 둔다. 지금은 게임이 링크할 모듈 묶음 타깃(`Source/Engine.Target.cs`)만
있고, 모듈은 [구현 계획](../Docs/Design/EngineImplementationPlan.ko.md)의 순서에 따라 채운다.

```text
Engine/
  Source/
    Engine.Target.cs      게임이 링크하는 런타임 모듈 묶음(Installed SDK 기본 타깃)
    Runtime/<Module>/     게임에 들어가는 엔진 모듈(<Module>.Build.cs는 EngineModuleRules 상속)
    Developer/<Module>/   도구·에디터 전용 모듈
    Editor/<Module>/      에디터 모듈
    ThirdParty/           외부 라이브러리 adapter
  Plugins/<분류>/<플러그인>/
    <플러그인>.rocksplugin   플러그인 기술자(후속)
    Source/<Module>/<Module>.Build.cs
```

Engine 모듈은 Core 모듈에 의존할 수 있지만 Core 모듈은 Engine 모듈을 참조하지 않는다. 전체 구성은
[저장소 구성](../Docs/Design/RepositoryLayout.ko.md)을 참고한다.
