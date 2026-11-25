# Sparks VS Code 확장

Sparks(`.spark`) 언어 등록·문법 강조와 언어 서버 `sparks-lsp`(진단·의미 강조·자동완성·hover·정의 이동·빠른 수정·서식) 연결이다. 저장소 안 개발용이며 마켓에 올리지 않는다. 설계는 [Docs/Design/SparksTools.ko.md](../../../Docs/Design/SparksTools.ko.md) 12절.

## 준비

1. 언어 서버 빌드(저장소 뿌리에서): `dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=SparksLsp`
2. 확장 빌드(이 폴더에서, Node.js 20 이상): `npm ci` → `npm run compile`
3. 설치: VS Code 명령 `Developer: Install Extension from Location...`에 이 폴더를 준다. 확장을 고치며 쓸 때는 이 폴더를 VS Code로 열고 F5(확장 개발 창).

## 설정

| 설정 | 기본 | 뜻 |
|---|---|---|
| `sparks.server.path` | 빈 글자 | 서버 실행 파일. 비우면 작업 공간 위의 Rocks 저장소에서 `out/dotnet-<플랫폼>-sparkslsp-<구성>/bin`의 가장 최근 빌드 |
| `sparks.server.schemas` | `[]` | 엔진 binding schema 폴더(`*.sparkbind.json`) — 상대 경로는 첫 작업 공간 폴더 기준 |
| `sparks.trace.server` | `off` | 프로토콜 기록(`Sparks` 출력 창) |

## 서버를 켠 채 다시 빌드하기

확장은 빌드 출력의 `SparksLsp`를 직접 실행하지 않는다. 확장 저장소의 칸(`server/<창 pid>/<내용 해시>/`)에 복사해 그것을 실행하므로, 편집기를 연 채 서버를 다시 빌드해도 링크가 잠긴 파일과 부딪히지 않는다. 원본이 바뀌면 새 칸을 만들어 서버를 갈아 끼운다(상태 표시줄에 `Sparks language server updated`). 알림이 빠지면 `Sparks: Restart Language Server` 명령이 같은 일을 한다.

## 한계

- 폴더를 열지 않고 연 `.spark` 파일은 분석하지 않는다(서식만 된다 — 단일 파일 모드 없음).
- 문법 강조는 서버 없을 때의 어휘 강조이고, 이름의 종류는 서버의 의미 강조가 칠한다.
