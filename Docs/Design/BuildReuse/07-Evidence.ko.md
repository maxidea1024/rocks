## 7. 근거 세부와 탐침

### 7.1 UE 대조(공유 저장소)

- UBA cache는 cmd key(애플리케이션 바이너리 CAS + 루트 정규화한 명령줄)로 bucket 안 항목들을 찾고, 항목마다 프로세스가 실제로 읽은 입력 파일 CAS 목록이 지금 파일과 맞는지 본다(`UBA/Common/Private/UbaCacheClient.cpp:627–760`, `UBA/Common/Public/UbaCacheEntry.h`). 이 문서의 2단 조회와 같다. Rocks는 컴파일러 depfile로 읽은 파일을 얻는다(UBA는 파일 접근 가로채기).
- bucket은 플랫폼·구성 해시이고 PCH 사용 action은 로컬 루트를 더한다(`UBT/Executors/UnrealBuildAccelerator/UBAExecutor.cs:1329–1370`). 이 문서의 경로 결속(BR15)이 대응한다. Rocks는 PCH 소비 TU를 결속하지 않는다 — PCH를 저장하지 않고 소비 TU만 저장하며, object 안 경로를 3.4.1절로 없앴기 때문이다.
- UBA는 link action도 기본으로 cache한다(`bCacheLinkActions = true` — `UBT/Executors/UnrealBuildAccelerator/UnrealBuildAcceleratorConfig.cs:371`, 1336행). Rocks는 link를 저장하지 않는다(BR18).
- UE의 결정적 빌드(`bDeterministic`, Shipping 기본)와 VFS(`bUseVFS`, 실험 단계)는 이 문서 3.4.1절 인자의 근거다.

### 7.2 UE 대조(메타 단계)

- UBT는 UHT 실행 전에 timestamp 파일과 UHT 입력 헤더·UHT 실행 파일 시각을 비교해 생성 코드가 최신이면 UHT를 띄우지 않는다(`UBT/System/UHTExecution.cs:896` `AreGeneratedCodeFilesOutOfDate`, 1394행, 1518행). UBT는 모듈마다 마지막 갱신 시각을 기록(TimestampsFile, 932–990행)하고 그중 하나라도 낡았으면 UHT를 실행한다. 기준은 시각이다. 헤더가 참조하는 외부 파일은 따로 외부 의존 파일로 기록한다(1042–1046행 `AreExternalDependenciesOutOfDate`) — 이 문서의 열거·존재 확인 기록(2.4절)의 선례다.
- Rocks는 모듈 단위로, 시각이 아니라 내용 해시와 의존 기록으로 판정한다(BR1·BR2). 리플렉션 헤더가 include한 헤더의 변화도 잡는다 — UHT는 자체 파서로 헤더 하나씩 읽고 include를 따라가지 않지만, Rocks MetaCompiler는 clang AST로 include한 타입 정보까지 쓰기 때문이다.

### 7.3 탐침

`%LOCALAPPDATA%\Rocks\out\toolchains\llvm-22.1.8\bin\clang-cl.exe`로 헤더 하나를 include하는 소스를 두 루트(`rootA`, `rootLonger_B`)와 실행마다 새 stage 디렉터리에서 `/c /Z7 /Od`로 컴파일했다.

| 인자 | 같은 루트 재실행 | 두 루트 | COFF timestamp | 남은 절대 경로 |
|---|---|---|---|---|
| 지금 인자 | 다름 | 다름 | 실제 시각 | 컴파일러, stage object, Windows SDK |
| + `/Brepro` | 다름 | 다름 | 0 | 같음 |
| + `-gno-codeview-command-line` | 다름 | 다름 | 0 | stage object |
| + `-ffile-prefix-map=<루트>=.`·`-fdebug-compilation-dir=.`·`-object-file-name=a.obj` | **같음** | **같음** | 0 | 없음 |

`rootLonger_B`에서 `rootA`의 object를 `lld-link /debug:full /pdbsourcepath:<rootLonger_B>`로 링크한 PDB에는 `rootLonger_B\src\a.cpp`가 있고 `rootA`는 없다.

- 탐침은 `-ffile-prefix-map`(디버그·매크로·coverage 매핑을 함께 바꿈)으로 했다. 설계는 `__FILE__`을 지금처럼 두려고 `-fdebug-prefix-map`만 더한다(3.4.1절). 디버그 정보에 대해서는 같은 매핑이지만, 실제 인자 조합의 byte 일치는 T2가 다시 확인한다.
- 탐침 스크립트와 결과는 세션 scratchpad에만 두었다(저장소에 넣지 않는다).
