## 7. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 타깃 종류 매크로 | `UE_GAME`·`UE_EDITOR`·`UE_SERVER`·`UE_CLIENT`·`IS_PROGRAM` | 없음. 빌드 주입 상수 | 기능 매크로(BD2)로 컴파일 분기가 충분하고, 실행 중 분기는 상수가 더 명확하다 |
| 서버·클라이언트 코드 값 | 타깃이 재정의 가능 | 타깃 종류로 고정 | SDK 변형·시험 행렬을 줄인다 |
| 매크로 기본값 | `WITH_EDITORONLY_DATA`는 정의가 없으면 1 | 기본값 없음, 미정의는 `#error` | 매크로를 넘기지 않은 컴파일 경로가 조용히 다른 조합이 되지 않게 |
| Program의 에디터 데이터 | 기본 1(시험 타깃만 0) | 기본 0, `WithEditorData`로 켬 | Rocks Program 대부분이 시험·벤치마크이고 SDK 조합을 Game과 공유한다 |
| Runtime 모듈과 Program | `Runtime`은 Program에서 빠지고 `RuntimeAndProgram`이 따로 있다 | Runtime은 Program에도 들어간다 | Rocks 시험·도구는 런타임 모듈을 그대로 링크한다 |
| Editor 모듈과 Program | `EditorAndProgram`은 모든 Program에 들어간다 | `CompileWithEditor` Program에만 들어간다 | Program이 에디터 코드를 쓰는지를 타깃이 명시한다 |
| 엔진 모듈 type | 엔진 모듈은 기술자 type으로 거르지 않는다 | 엔진 모듈도 type을 갖고 도달 검사한다 | 서버·클라이언트 전용 엔진 모듈을 검사로 막는다 |
| EditorOnly 표시 | `WITH_EDITORONLY_DATA` 블록 안 필드에 자동으로 붙는다 | 명시해야 하고 블록과 양방향 일치를 검사한다. 두 조합 비교를 더한다 | 지정자 하나를 보고 저장 의미를 알 수 있게 |
| 모듈 type 사이 의존 | 규칙 없음(타깃 빌드 때 깨진다) | 규칙 로드 때 포함 관계 검사(BD25) | 특정 타깃에서만 드러나는 실패를 미리 막는다 |
| 명시 참조된 플러그인의 미지원 플랫폼 | 참조에 복사된 지원 플랫폼으로 조용히 뺀다(`UEBuildTarget.cs:5847`) | 오류(BD13) | 명시한 의도와 다른 결과를 조용히 만들지 않는다 |
| 프로젝트 조건으로 꺼진 플러그인을 다른 플러그인이 의존 | 의존 처리가 켠다(`UEBuildTarget.cs:5966-5975`) | 오류(3.6절) | 프로젝트가 끈 것을 다른 경로가 조용히 켜지 않게 |
| 쿠킹 데이터만 읽기 | Game·Client·Server는 쿠킹 데이터만(`bBuildRequiresCookedData`) | 채택하지 않음. 에디터 패키지의 `EditorOnly` 값을 건너뛴다 | Development Game의 개발 흐름 |
| include-only 의존 | 있음 | 없음 | 순환 금지 규칙이 있다 |
| 플러그인 조건 | 허용·거부 목록, optional, 지원 플랫폼 복사, 플러그인 의존 참조에도 조건 | 허용 목록만, optional 없음, 지원 플랫폼은 플러그인 한 곳, 의존 참조에는 조건 없음 | 충돌 규칙 제거, 빌드 결과가 저장소 상태에 조용히 의존하지 않게, 조건을 프로젝트 한 곳에 |
| 기술자 `AdditionalDependencies` | 있음 | 없음 | 의존의 단일 출처 |
| 리플렉션 타입 정보 export | 클래스를 export하지 않으면 `MinimalAPI`가 있어야 타입 정보가 export된다 | 생성 코드가 항상 export한다(BD28) | 지정자를 빠뜨려 Editor 빌드에서만 다른 모듈 참조가 깨지는 일을 없앤다 |
| 공개 헤더의 정적 저장소 | 관례에 맡긴다 | 정책으로 막는다(BD29) | 모듈마다 상태가 갈라지는 오류는 Editor(`Modular`)에서만, 그것도 실행 중에만 드러난다 |
| 서드파티 정적 라이브러리 소비 | 소비 모듈의 바이너리마다 링크된다 | 같다. 단 `HasGlobalState` 서드파티는 직접 소비 모듈을 하나로 제한한다(BD29) | 전역 상태가 두 벌이 되지 않게 |
| 플러그인을 켜는 타깃 목록 | `EnablePlugins`·`DisablePlugins`·`OptionalPlugins`·`BuildPlugins` 목록 넷 | `Plugins` 하나(`enabled` 값) | 프로젝트 `plugins[]`와 형식을 하나로 맞춘다 |
| 플러그인의 지원 프로그램 | `.uplugin` `SupportedPrograms`에 없으면 Program이 무시 | 없음. 타깃이 켜면 들어간다 | 켜는 쪽 한 곳에서 정한다(이중 등록 없음) |
| 끈 플러그인 모듈 의존 | 경고 | 오류 | 조용히 빌드되는 모듈을 막는다 |
