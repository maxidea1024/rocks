## 6. 에셋 타입·객체 통합·참조

### 6.1 객체 통합

- 로드된 쿠킹 패키지·에셋은 `AssetManager`가 소유하는 **에셋 객체 Registry**(고유 ContextId, 엔진 TypeRegistry 공유)에 있다. 에디터 패키지는 AssetDatabase `EditorPackageSet`이 소유하는 **에디터 객체 Registry**(같은 패키지 정책 Registry·`ReferenceObserver`)에 있다(7.1절 3 — AssetManager·AssetLoader는 쿠킹 매니페스트 전용이다). World Registry와 분리한다(여러 World·Tryout 공유, 에셋끼리 ObjectRef 가능). 접근은 `AssetManager::With`(에셋 문맥 ObjectAccessScope를 중첩해 연다).
- `Package : Object`(Outer·이름 없음 — 정체성은 AssetId): `ROCKS_PROPERTY(Transient, ReadOnly) AssetId id`, 플래그, 하드 의존 패키지 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Package>` — AssetManager 조회용 사본이며 GC 보존 역할은 없다, 존치 여부는 A1이 정한다), bulk 접근자(`ROCKS_NATIVE`: 에디터 패키지는 bulk 저장소 — 파일 경로 + 로드 지문 + BulkIndex로 지연 읽기를 하고 필요할 때 파일을 다시 연다(UE `FEditorBulkData`의 PackagePath + offset), 저장 성공 후 새 파일로 다시 잇는다, 5.5절·7.1절 15). 소속 객체(export)는 목록이 아니라 Registry 자식 고리가 붙잡고(패키지 정책 Registry의 outer → inner 강한 간선 — [ObjectFoundations](../ObjectFoundations.ko.md) 1.7절) export 순서는 `CollectInners(DepthFirst)`다. `Asset : Object`(`ROCKS_CLASS(Abstract)`)의 Outer는 Package. inner → outer 강한 간선(계획 §5.2)과 패키지 정책의 outer → inner 강한 간선이 함께 있다.
- 저장 속성은 `ROCKS_PROPERTY`. 대용량 데이터는 `ROCKS_STRUCT BulkDataRef`(bulkId 1부터·rawBytes·`Sha256Digest`) 필드이고 내용은 Bulk 섹션. 쿠킹 플랫폼 데이터의 mip별 entry도 bulkId로 가리킨다(파일 offset은 BulkIndex만 안다).
- 대표 에셋의 AssetId는 `ROCKS_PROPERTY(Transient, ReadOnly) AssetId`이며 로드 때 `prepare` hook이 Commit 전에 설정한다.
- 에셋 객체 Registry의 모든 객체는 Package이거나 Outer 체인이 Package에 닿는다(Registry 정책 — 패키지 없는 객체 생성은 거부한다). 그래서 모든 하드 간선에는 소유 패키지가 있다. 패키지는 이름이 없고(정체성은 AssetId), 이 Registry의 `RequestDestroy`는 Unsupported라 소속 객체는 Discard로만 죽는다(ObjectFoundations 1.3·1.7절).
- 패키지 사이 하드 간선의 변경: 에셋 객체 Registry는 서로 다른 패키지에 속한 객체 사이의 `ObjectRef`·`ObjectRefArray` 원소·`StructRefArray` 원소 참조 멤버·`ObjectRefMap` 값(리플렉션 B9f4)의 설정·해제를 관찰자(A0-O `ReferenceObserver` — 순 다중도 합산 표, [ObjectFoundations](../ObjectFoundations.ko.md) 3절)로 알린다. 규칙:
  - 의존 계수는 (소유 패키지, 대상 패키지) 쌍의 간선 다중도다. 필드 둘이 같은 대상을 가리키면 2이고 하나를 지워도 1이 남는다.
  - 관찰하지 않는 간선: 공개 전 객체(`ConstructionContext` 안 — Commit할 때 Registry가 공개하는 객체들의 패키지 사이 간선 다중도를 한 번에 보고한다. 로드·임포트·`asset-new`·복제가 모두 이 경로다), `DestroyRequested` 소유자의 간선 제거(Discard 정리 — Discard가 부분 트리에서 나가는 계수를 관찰자에 음수 다중도로 한 번 보고하고 AssetManager는 그것으로 반납한다), 패키지 객체가 소유한 간선 전부(Package의 하드 의존 목록 `ObjectRefArray<Package>` — AssetManager가 직접 유지하는 거울이다).
  - 관찰자는 Registry가 채우는 순 다중도 합산 표다(잠금 안에서 사용자 코드를 부르지 않는다). `AssetLoader::Pump`가 Closing 처리 전에 `TakeChanges`로 꺼내 묶음 전체를 적용한 뒤 계수·Package 의존 목록·dirty를 판정한다(꺼낸 순서는 키가 처음 생긴 순서라 묶음 중간의 합은 음수일 수 있다).
  - 새 간선이 상주 단위 사이에 순환을 만들면 두 단위(와 순환 경로의 단위)를 하나로 합친다(union-find, 11.4절). 합친 단위는 Discard될 때까지 나누지 않는다(보수 규칙 — 계수가 서로를 붙잡지 않는다).
  따라서 도구·에디터 서비스가 바꾼 하드 간선도 수명과 저장에 반영된다.
- GC: 에셋 객체 Registry의 증분 GC는 EngineAssets가 프레임 끝에 한 조각, 도구는 명시 `Collect`. GC는 열린 `ConstructionContext`가 없을 때만 진행한다(11.2절). 종료: World 종료 → lease·scope 해제 → 에셋 객체 Registry Shutdown.

### 6.2 참조 종류

| 종류 | 필드 | 저장 | 로드 | 쿠킹 | 누락 | 타입 불일치 | 순환 |
|---|---|---|---|---|---|---|---|
| Hard | `ObjectRef<T>` / `ObjectRefArray<T>`(다른 패키지 객체) | ObjectImports + Dependencies(Hard) | 같은 로드 작업에서 먼저 준비, 실패 시 참조자도 실패 | 전이 포함 | 저장·`set` 거부, 로드 `MissingDependency` | 저장 거부, 로드 실패 | SCC 함께 로드·상주(11.3·11.4절) |
| Soft | `SoftObjectRef<T>` / 배열 | Dependencies(Soft) | 자동 로드 안 함 | 기본 포함(`CookSoftReferences`) | 저장 경고(`Required`면 거부), 로드 요청 NotFound | `RequestAsset` TypeMismatch | 허용 |
| EditorOnly | `EditorOnly` 표시 Hard·Soft 필드 | Dependencies(EditorOnly) | 런타임 로더 무시 | 제외, 쿠킹 데이터에서 필드 제외 | 에디터 진단만 | 에디터 진단만 | 허용 |
| Build | `DependencyDeclarer`가 선언(16.2절) | Dependencies(Build) | — | 산출물 선행 조건·너겟 키 포함 | 쿠킹 실패 | 쿠킹 실패 | 지원 SCC는 한 단위, 아니면 `UnsupportedCycle` |
| ImportOwner | ImportRecord `ownerAssetId`(다중 출력의 소유 패키지) | Dependencies(ImportOwner) | 런타임 로더 무시 | 제외 | 에디터 진단(`MissingDependency`) | — | 허용 |

동적으로 로드하는 에셋은 `CookSettings`의 AlwaysCook·PrimaryAsset 규칙에 명시한다.

### 6.3 World·게임플레이에서의 참조

World 객체는 에셋 객체 Registry 밖에 있어 `ObjectRef<에셋>`을 걸 수 없다. World 객체의 영속 참조는 `SoftObjectRef<T>`(필드), 런타임 보유는 `AssetLease`(`ROCKS_NATIVE` 멤버)다. "하드" 의미(함께 로드·쿠킹)는 그 World 데이터를 담는 레벨·prefab 패키지(후속)의 Dependencies(Hard)로 표현한다. prefab 패키지는 새 섹션 없는 일반 에셋 패키지이고(대표 에셋 `PrefabAsset`의 값으로 원형 본문을 담는다), 본문의 Soft 참조와 기준·중첩 prefab을 저장 hook이 Hard로 내준다([Prefab](../Prefab/04-StoragePropagationRuntime.ko.md) 3.6절). 에셋이 다른 에셋을 런타임에 붙잡아야 하면 lease가 아니라 Hard 참조를 쓴다(11.4절). 계획 §7.1의 `ObjectRef<MaterialAsset>` 예시는 이 규칙으로 바꾼다(17절).
