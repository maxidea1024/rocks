# Core — 기반 라이브러리

엔진이 아니라 엔진·도구·게임이 함께 쓰는 바탕 계층이다. 모듈은 `Source/Runtime/<Module>/`에 있고 각
폴더에 `<Module>.Build.cs`·`Public/`·`Private/`·`Tests/`가 있다. 전체 저장소 구성은
[저장소 구성](../Docs/Design/RepositoryLayout.ko.md)을 참고한다.

| 모듈 | 의존 | 내용 |
|---|---|---|
| Base | — | 원시 타입·Guid POD·ABI(헤더만) |
| PlatformBase | Base | 메모리·파일·스레드·시계(공개 인터페이스는 플랫폼 독립, 구현은 `Private/<플랫폼>/`) |
| Core | PlatformBase | 컨테이너·할당기·문자열·서식·수학·진단·시간 |
| Task | Core | 작업 DAG·worker scheduler |
| Serialization | Core | Archive 계약·버전 규칙 |
| MetaSchema | Core | 타입·필드 메타 schema(헤더만) |
| Localization | Core | 지역화 Text·catalog |
| ObjectRuntime | MetaSchema, Task | Object·Registry·GC·Reflection |
| ObjectSerialization | ObjectRuntime, Serialization | 객체 그래프 직렬화 |

`Source/Developer/TestSupport/`는 테스트 지원 코드다. 테스트 실행 파일은 `Programs/FoundationTests`다.
