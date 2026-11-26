## 3. 개념과 모듈

| 개념 | 모듈·타입 | 상태 |
|---|---|---|
| Source File | 원본 루트(4.5절) | 구현 |
| Asset | AssetRuntime `Asset : Object`(Outer = `Package`), A5 `Texture` 계열 | 구현 |
| Asset Package | AssetRuntime `Package : Object`, `PackageWriter`·`PackageReader` | 구현 |
| Importer | AssetDatabase `Importer`, TextureTools `TextureImporter` | 구현(텍스처) |
| Import Metadata | 에디터 패키지 `ImportRecord` 섹션 | 구현 |
| Asset Registry(인덱스) | AssetDatabase `AssetIndex` | 구현 |
| 에셋 너겟 | AssetNugget | 구현(로컬 — `NuggetFileSystemBackend`), `NuggetRockpileBackend`는 계약(Rockpile 서버 R0–R2는 별도 — [Rockpile](../Rockpile.ko.md)), C++ 직접 원격 backend 없음 |
| Cooker | AssetCooker | 구현(비압축 형식), 블록 압축 계약 |
| Runtime Asset Loader | AssetRuntime `AssetLoader` | 구현(CPU ready), `Render` ready 계약 |
| Asset Manager | AssetRuntime `AssetManager` | 구현(scope·PrimaryAsset·bundle·즉시 해제), 예산 LRU 해제 후속 |
| 메시·스켈레톤·애니메이션 에셋 | `MeshSource`·`Mesh`·`Animation`(Runtime), `MeshTools`·`AnimationTools`(Developer) | `MeshSource` 구현(MA1 — 원본 메시 저장·검증·위상 조회, bulk 쓰기는 `MemoryBulkSource`), `Mesh`(빌드 설정 값 타입 `MeshBuildSettings`·`SkinnedMeshBuildSettings`와 단순 충돌 값 타입 `SimpleCollision`·검증 — MA3a)·`MeshTools`(정적 메시 LOD 빌드 순수 함수 `BuildStaticMeshLod`, ThirdParty MikkTSpace 패치) 구현(MA2a — 지금 의존은 Mesh → ObjectRuntime·CoreTypeBindings·AssetRuntime, MeshTools → MeshSource·Mesh·Animation·DeterministicMath·MikkTSpace, 아래 그래프의 나머지는 MA2b 이후), 스킨 메시 LOD 빌드 순수 함수 `BuildSkinnedMeshLod`(영향 줄이기·bone map·inverse-bind·morph 변화량) 구현(MA5a), `Animation`(본 계층 `BoneHierarchy`·`Skeleton` 에셋 클래스·검증·병합 판정 `MergeBoneHierarchy`) 구현(MA4a), 클립 `AnimationAsset`·`AnimationClip`·원본·쿠킹 데이터 v1·쿠킹 변환 `BuildClipPlayback` 구현(MA6a — 지금 의존은 Animation → AssetRuntime·CoreTypeBindings·Curves), 나머지 설계([MeshAnimationAssets](../MeshAnimationAssets/08-Modules.ko.md) 8절·3.7·4.8·5.4·6.4·6.5·7.5절, MA2b·MA4b·MA5b·MA6b·MA7) |
| 외부 엔진 교환 임포터 | 엔진 플러그인 `UnrealExchange`(Developer) | UX1a 리더·경로 대응·임포트 순서 구현, 나머지 설계([Unreal 교환 형식](../UnrealExchange.ko.md) 10절, UX1–UX8) |

이름 구분: 에셋 객체가 사는 `ObjectRegistry`는 **에셋 객체 Registry**, 에디터 패키지 인덱스는 **AssetIndex**(UE AssetRegistry 대응), 쿠킹 결과의 런타임 목록은 **쿠킹 매니페스트**(`CookedManifest.rman`, 10절)라 부른다.

의존 방향(→는 의존):

```
Programs/Commandlet → TextureTools → { AssetCooker, AssetDatabase, AssetNugget, ImageCodec, Texture }
AssetCooker → { AssetDatabase, AssetNugget }        AssetDatabase(Developer) → { AssetRuntime, HostConfig }
Texture(Runtime) → { AssetRuntime, GraphicsTypes }       ImageCodec(Developer) → { Core, Compression }
AssetRuntime → { ObjectSerialization, ObjectRuntime, Serialization, Task, Compression }
EngineAssets(Runtime, 신규) → { Engine, AssetRuntime }   — AssetManager를 EngineSubsystem으로 붙이는 연결 모듈
MeshSource(Runtime) → { AssetRuntime }                  Animation(Runtime) → { AssetRuntime, Curves }      (설계, MeshAnimationAssets 8절)
Mesh(Runtime) → { AssetRuntime, MeshSource, Animation, Material, GraphicsTypes }
MeshTools(Developer) → { Mesh, MeshSource, AssetCooker, AssetDatabase, AssetNugget }   AnimationTools(Developer) → { Animation, AssetCooker, AssetDatabase, AssetNugget }
UnrealExchange(플러그인, Developer) → { AssetDatabase, TextureTools, MeshTools, AnimationTools, MaterialCompiler, Material }   (설계, UnrealExchange 10.1절)
```

AssetRuntime은 Engine을 모른다. 도구는 Engine을 링크하지 않고 `AssetManager`를 직접 소유한다. 게임은 `EngineAssets`가 AssetManager를 `EngineSubsystem`으로 등록하고 프레임 단계(로드 결과 전달·에셋 객체 Registry GC 한 조각)를 연결한다. World는 AssetRuntime에 의존하지 않는다(6.3절).
