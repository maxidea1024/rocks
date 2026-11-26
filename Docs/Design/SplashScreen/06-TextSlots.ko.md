# 시작 화면(스플래시) — 6. 글자 칸과 배치

[목차](README.ko.md)

## 6. 글자 칸과 배치

### 6.1 칸

| 칸 `SplashTextSlot` | 내용 | UE |
|---|---|---|
| `Title` | Studio: `Rocks Studio — <프로젝트>`(프로젝트 없으면 `Rocks Studio`). 게임: 설정 `title`, 비면 프로젝트 이름(`AppInfo::ProjectName`), 엔진 프로그램은 앱 이름 | `GameName` |
| `Version` | `Rocks Engine <버전>`. 비출시 구성은 ` (<branch> <commit 8자>)`를 붙인다(`build::Description()`) | `VersionInfo1` |
| `Progress` | 진행 문구(7.3절) | `StartupProgress` |
| `Copyright` | 설정 `copyright` | `CopyrightInfo` |
| `Custom1`–`Custom6` | 프로젝트 코드가 Show 전에 배치와 함께 등록 | `User1`–`User6` |

막대(`SplashProgressBar`)는 칸이 아니라 따로 둔다(UE에는 없다).

### 6.2 기본 배치

이미지 아래 가장자리와 좌우 가장자리 기준 논리 px다. 이미지 크기와 상관없이 같은 오프셋이다(UE처럼 아래 기준).

| 칸 | 기준 | 위치 | 글꼴 | 색(sRGB) |
|---|---|---|---|---|
| `Title` | 왼쪽 아래 | x 32, 아래에서 84 | 20 px SemiBold | `#FFFFFF` |
| `Version` | 왼쪽 아래 | x 32, 아래에서 55 | 12 px Regular | `#A8A8A8` |
| `Progress` | 왼쪽 아래 | x 32, 아래에서 34, 폭은 가운데까지 | 12 px Regular | `#D6D6D6` |
| `Copyright` | 오른쪽 아래, 오른쪽 맞춤 | 오른쪽 32, 아래에서 33 | 11 px Regular | `#808080` |
| 막대 | 아래 | 좌우 32, 아래에서 12, 높이 2 | — | 바탕 흰색 12%, 채움 Warm Ivory `#F5F2EB` |

- **이미지 약속.** 아래 96 논리 px는 글자 띠다. 이미지는 이 띠를 어둡고 고요하게 둔다(기본 이미지 셋이 지킨다 — 12절). 밝은 이미지를 쓰는 프로젝트는 설정으로 색을 바꾼다.
- 글꼴: Windows Segoe UI(SemiBold는 `Segoe UI Semibold`), ClearType, 높이는 모니터 배율로 키운다. CJK는 GDI 글꼴 연결에 맡긴다. macOS는 시스템 글꼴(`NSFont systemFontOfSize`·`weight`). UE의 8방향 외곽선은 두지 않는다(띠가 어둡다는 약속으로 대신 — 외곽선이 필요한 프로젝트는 칸 설정 `outline`).
- 게임과 Studio의 모의 화면: [Studio](../img/SplashScreen/studio-mock.png), [엔진 기본 게임](../img/SplashScreen/game-mock.png), [템플릿](../img/SplashScreen/template-mock.png).

### 6.3 칸별 기본 표시

| 칸 | Studio | 게임·엔진 프로그램 | 근거 |
|---|---|---|---|
| `Title` | 켬 | 켬 | UE는 사용자 정의 이미지면 게임 이름을 끈다 — Rocks는 설정 하나(`showTitle`)로 둔다 |
| `Version` | 켬 | 끔 | UE와 같다 |
| `Progress` | 켬 | 끔 | UE와 같다(최종 사용자에게 내부 단계 이름은 의미가 적다). 설정 `showProgressText`로 켠다 |
| `Copyright` | 켬(값이 있을 때) | 켬(값이 있을 때) | UE와 같다 |
| 막대 | 켬 | **켬** | UE 개선(요구 2). 설정 `showProgressBar`로 끈다 |

### 6.4 그리기 규칙

- 글자는 칸 폭을 넘으면 끝을 말줄임(`…`)으로 자른다(Windows `DT_END_ELLIPSIS`, macOS `NSLineBreakByTruncatingTail`). 줄바꿈은 `Custom` 칸의 `wordWrap`만.
- 바뀐 칸과 막대 영역만 다시 그린다. 이미지는 창 크기 DIB(macOS `CGImage`) 하나로 미리 만들어 둔다.
- 칸 배치 덮어쓰기(후속 — SP1은 기본 배치만, 개정 2. 보이기는 10.1절 `show_*`만 정한다): 칸마다 `anchor`·`x`·`fromBottom`·`width`·`size`·`weight`·`color`·`align`·`outline`, 막대의 `margin`·`fromBottom`·`height`·`trackColor`·`fillColor`. 값 범위는 등록 지정자로 막는다(위치는 0–논리 크기, 크기 8–48 px).
