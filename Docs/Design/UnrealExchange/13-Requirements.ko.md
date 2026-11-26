## 13. 다른 문서에 반영할 요구

| ID | 대상 | 요구 | 상태 |
|---|---|---|---|
| UXR1 | [에셋 시스템](../AssetSystem/12-Textures/12.1-TypesAndSources.ko.md) 12.2절 | (2026-11-23 대체) JPEG 원본 허용 — 파일·Unreal 교환 모두 옵션 없이 받는다, 처음의 `--allow-lossy-origin`·`LossyOriginAccepted`·ImportRecord `lossyOrigin` 표시 | 반영(AssetSystem 개정 10, 12.2절) |
| UXR2 | 에셋 시스템 7절(AssetIndex)·5.2절 Tags | 임포터가 정하는 태그 키(`UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths`)를 기록하고 검색할 수 있게 한다 | 반영(AssetSystem 개정 10, 7절 — 임포터 키는 `<임포터 이름>.` 접두) |
| UXR3 | 에셋 시스템 4.6절 | 10.3절 진단 코드 등록 | 반영(AssetSystem 개정 10, 4.6절) |
| UXR4 | [머티리얼 시스템](../MaterialSystem.ko.md) 5.4·9.2절 | 같은 이름 파라미터 병합·함수 global 파라미터 끌어올리기(8.5절)를 번역 결과가 V8을 통과하는 형태로 받는지 확인. 엔진 내장 "구운 표면" 부모 머티리얼 정의(엔진 콘텐츠 `/Engine` 마운트는 후속이므로 그 전까지 두는 곳 포함) | 반영(MaterialSystem 개정 6, 9.2·11.3절) |
| UXR5 | 정책 검사(Rocks.Tooling.Checks) | `Extras/` 아래에 Rocks 모듈 규칙·서술자가 없음을 확인 | 구현(2026-11-20, 11.1절 `foreign-engine-boundary`) |
| UXR6 | 에셋 시스템 시험 기반 | 결정적 AssetId 발급기 주입 지점(시험 전용) | 반영(AssetSystem 개정 10, 14.1절) |
| UXR7 | [빌드 대상](../BuildTargets/README.ko.md) | 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`의 서술자·종류(Developer)·Shipping 제외 | 반영(BuildTargets 개정 3, 3.6절) |
| UXR8 | 에셋 시스템 7.1절 18(A2f)·5.4절 ImportRecord | 임포터 태그: 임포터가 정한 태그(키는 `<임포터 이름>.` 접두)를 남길 길. 지금 저장은 Tags를 타입 hook으로만 다시 만들어(7.1절 15) 임포터가 `UnrealExchange.ObjectPath`·`FormerPaths`를 넣을 수 없다. 권장: ImportRecord에 임포터 태그 목록을 두고 저장이 타입 hook 태그와 합쳐 쓴다(UE `UAssetImportData`가 AssetRegistry 태그를 내는 것과 같다 — 원천이 기록이라 플러그인을 끈 프로젝트에서 다시 저장해도 남는다), 문맥 `AddTag(key, value)`, 재임포트는 옛 기록의 태그를 읽을 수 있게(옛 경로 누적) | 요구(2026-11-23, 에셋 세션에 넘김) |
| UXR9 | 에셋 시스템 7.1절 18(A2f) | 가변 원본: 9.2절은 에셋 JSON(역할 `asset`)과 blob마다 역할 `blob:<sha256>`을 기록한다. 지금 임포터 역할은 고정 목록이고 재임포트는 기록 역할만 바꿀 수 있다. 권장: 임포터가 역할 접두(`blob:`)를 선언하면 그 접두의 역할을 개수 제한 안에서 받고, 재임포트가 원본 목록 전체를 바꿀 수 있게 한다. 원본 합 상한(2 GiB)을 넘는 에셋은 실패로 둔다(한계) | 요구(2026-11-23, 에셋 세션에 넘김) |
| UXR10 | 에셋 시스템 7.1절 18(A2f) | 기존 에셋 참조: 임포터가 이미 있는 다른 에셋(이번 실행에서 앞서 임포트한 것 포함)을 AssetId로 열어 출력의 Hard 참조로 걸 길(머티리얼 → 텍스처, 메시 → 머티리얼, 스킨 메시·클립 → 스켈레톤). 지금 문맥은 자기 출력끼리만 참조할 수 있다 | 요구(2026-11-23, 에셋 세션에 넘김) |
| UXR11 | 에셋 시스템 7.1절 18(A2f)·6.2절 | 다중 에셋 원자 저장: 서로 Hard로 참조하는 여러 주 출력(각자 소유 기록)을 한 저장 묶음으로 저장(9.3절 Hard 순환 단위). 그 전까지 구성원이 둘 이상인 단위는 `HardCycle`로 실패한다(10.5절 8번) | 요구(2026-11-23, 에셋 세션에 넘김) |
