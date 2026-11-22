# RocksExport (Unreal Engine 5.8 플러그인)

UE 에디터의 에셋을 Rocks가 읽는 **Unreal 교환 묶음**(`<이름>.rxbundle/`)으로 내보낸다. 설계는 Rocks 저장소
`Docs/Design/UnrealExchange.ko.md`(3절 형식, 9절 식별·의존, 11절 플러그인)다. 지원 UE 버전은 5.8 하나다.

현재 단계는 UX2a(텍스처 추출)다. 묶음 목록·에셋 JSON 공통부(objectPath·class·ueClass·dependencies·diagnostics)·의존 닫힘·
redirector와 텍스처 본문(설정 덤프, 원본 mip 사슬 blob — 설계 4.5절)을 쓴다. 메시·머티리얼·애니메이션 본문은 그 단계가 채운다.

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
