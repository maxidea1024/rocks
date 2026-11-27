# RocksExport (Unreal Engine 5.8 플러그인)

UE 에디터의 에셋을 Rocks가 읽는 **Unreal 교환 묶음**(`<이름>.rxbundle/`)으로 내보낸다. 설계는 Rocks 저장소
`Docs/Design/UnrealExchange/`(3절 형식, 9절 식별·의존, 11절 플러그인)다. 지원 UE 버전은 5.8 하나다.

현재 단계는 UX4a(머티리얼 계열 추출)다. 묶음 목록·에셋 JSON 공통부(objectPath·class·ueClass·dependencies·diagnostics)·의존 닫힘·
redirector, 텍스처 본문(설정 덤프, 원본 mip 사슬 blob — 설계 4.5절)과 정적 메시 본문(LOD별 원본·UE 준비 원본·생성 LOD,
충돌 — 설계 5.4절), 스켈레톤·스킨 메시 본문(본 계층·커브, 스킨 가중치·morph, 생성 LOD, 머티리얼 슬롯 — 설계 6.3절), 애니메이션 클립 본문(본 트랙·커브·이벤트 — 설계 7.1절),
머티리얼 계열 본문(그래프 노드·출력 핀·인스턴스 해석 값 — 설계 8.6절)을 쓴다.

## 빌드

Rocks 빌드와 검사는 이 폴더를 보지 않는다(`Extras/`). UE 설치판으로 빌드한다.

```bat
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
  -Plugin=<Rocks 저장소>\Extras\UnrealExport\RocksExport\RocksExport.uplugin ^
  -Package=<출력>\RocksExport -TargetPlatforms=Win64
```

## 설치

패키지 결과(`<출력>\RocksExport`)를 UE 프로젝트의 `Plugins\RocksExport`로 복사하거나 링크하고 프로젝트에서 켠다.

## 사용

- 에디터: 콘텐츠 브라우저에서 에셋을 고르고 우클릭 "Export to Rocks..." → 묶음 디렉터리를 고른다(의존 닫힘 포함).
- commandlet:

```bat
UnrealEditor-Cmd.exe <프로젝트>.uproject -run=RocksExport -Assets=/Engine/BasicShapes/... -Out=D:\Export\Shapes.rxbundle ^
  -unattended -nullrhi -nopause
```

| 인자 | 뜻 |
|---|---|
| `-Assets=` | 쉼표로 나눈 선택: 객체 경로(`/Game/A.A`), 패키지 경로(`/Game/A`), 폴더 재귀(`/Game/Dir/...`) |
| `-Out=` | 묶음 디렉터리. 이미 있으면 묶음이거나 빈 디렉터리여야 한다(다른 디렉터리는 덮지 않는다) |
| `-Closure` / `-NoClosure` | 범위 안 Hard 의존 닫힘을 함께 내보낼지(기본 `-Closure`) |
| `-SelfTest` | SHA-256·숫자 서식·JSON·UPROPERTY 덤프·writer·결정성 자기 시험 |

자기 시험은 UE Automation 시험 `RocksExport.*`로도 돈다.

## golden 묶음

Rocks 저장소의 시험용 golden 묶음(`Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/Golden.rxbundle`)은 이 플러그인으로
만든다. 원본 콘텐츠를 만드는 스크립트는 `Scripts/Golden/`(`make_column_gltf.py`, `make_golden_content.py`)에 있고, 차례와
내보내기 명령은 설계 12.3절이다. 플러그인 출력 형식을 바꾸면 golden도 다시 만든다.

Substrate golden(`GoldenSubstrate.rxbundle`)은 `r.Substrate=True` 프로젝트에서 `make_golden_content.py substrate` 단계로
만들고 `-nullrhi`로 내보낸다. 굽기 golden(`GoldenBaked.rxbundle`)은 `-AllowCommandletRendering -BakeMaterials`로
렌더링되는 실행에서 내보낸다. 둘 다 설계 12.3절에 명령이 있다.

## 굽기

`-BakeMaterials [-BakeSize=<n>]`(기본 1024, 1–8192)를 주면 Surface 영역 머티리얼·인스턴스의 속성 9개를 구워
`data.baked`에 넣는다(설계 8.9절). 렌더링이 필요하므로 `-AllowCommandletRendering`을 주고 `-nullrhi`를 빼야 한다.
