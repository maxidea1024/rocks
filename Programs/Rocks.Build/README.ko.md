# Rocks.Build native 실행기

`NativeBuildEngine.BuildAsync`는 명시한 모듈 그래프에서 C++23 compile → static archive → executable link action을 만들고 실행한다. CMake와 Python을 호출하지 않는다. 호스트 도구·C# 규칙 컴파일, Clang metadata 생성과 SDK/game 조립은 상위 CLI가 담당한다. `NativeCompileProfile.Arguments`가 native compile과 AST 생성의 공통 target 옵션이다.

## 현재 지원 범위

- Windows clang-cl/x64와 macOS clang++의 Debug/Development/Release/ASan(구성별 옵션은 [빌드 구성](../../Build/README.md#빌드-구성)). Release는 ThinLTO이며 Windows에서 llvm-lib·lld-link를 요구한다. ThinLTO는 코드 생성 옵션이라 `NativeCompileProfile`(AST 프로필)이 아니라 compile action에만 붙는다. 모듈별 `NativeModuleInput.Optimization`은 최적화 여부만 바꾸고 compile action identity에 포함된다. Windows ASan은 선택한 x64 compiler runtime DLL 및 같은 디렉터리의 import/thunk를 함께 사용한다. Mac deployment target은 `MACOSX_DEPLOYMENT_TARGET` 또는 명시 기본 13.0이며 compile/link/ABI 신원에 동일하게 적용한다.
- 모듈 dependency는 public include/define을 전파한다. private include/define은 owner에서만 사용한다. 현재 dependency 자체의 public/private 구분은 없다. 정적 의존의 PrebuiltLibraries/AdditionalLinkArguments는 최종 link로 전달한다.
- 소스가 없는 static module은 header-only다. 가짜 archive를 만들지 않고 ModuleOutputs에도 기록하지 않는다. DLL/shared library, PCH, Unity, remote executor, artifact cache 서버는 아직 구현하지 않았다.
- `Identity`는 전체 compiler 진단용이며 설치 경로가 들어갈 수 있다. `AbiIdentity`는 Installed SDK용 compiler version·effective triple(MSVC 환경 버전 접미 제거)·architecture·SDK version/build·deployment의 경로 독립 digest다(`--version`의 `Target:` 줄은 Apple clang에서 호스트 커널 버전이라 쓰지 않는다). 실제 증분 key에는 별도로 compiler/archiver/linker 및 선언 SDK 경로의 내용과 실행 환경을 포함한다.
- discovery는 툴체인 신원도 채운다: `CompilerVersion`(`--version` 첫 줄, 경로·URL을 담은 괄호는 commit hash만 남김), `TargetTriple`(`-print-effective-triple`), `CompilerDigest`·`LinkerDigest`(Windows 컴파일러·`LinkerPath`, Android NDK clang·`ld.lld`, macOS `xcrun --find clang++`·`ld`+`libLTO.dylib`), `OwnedRoots`(`resource`·`runtime`·`sysroot`·`ndk`), 정렬된 `AbiFields`, `RemovedEnvironment`. 수동으로 만든 툴체인은 이 필드가 비어 있으며, Core에 닿는 실행 파일을 빌드하면 ArgumentException이다.

## 빌드 환경 위생

`Rocks.Tooling.Common/NativeEnvironment`(와 Android discovery)는 프로세스 환경을 물려주지 않고 **허용 목록**의 변수만 정규 철자로 담은 새 환경을 만든다. Keyed 변수(Windows `INCLUDE`·`LIB`·`VCToolsInstallDir`·`WindowsSdkDir` 등 VS·SDK 환경, macOS `DEVELOPER_DIR`·`SDKROOT`·`MACOSX_DEPLOYMENT_TARGET`·`ZERO_AR_DATE` 등, 모두 `SOURCE_DATE_EPOCH`)만 증분 key(`Fingerprints.Toolchain`)에 들고, Runtime 변수(`PATH`·`TEMP`·시스템 폴더·`ROCKS_CLANG_FORMAT` 등)는 넘기되 key에서 뺀다 — 도구는 절대 경로와 바이너리 digest로 key에 있다. 그래서 실행마다 바뀌는 `DOTNET_CLI_TELEMETRY_SESSIONID`(dotnet msbuild) 같은 변수가 빌드를 무효화하지 않는다. 컴파일러·링커 동작을 바꾸는 것으로 알려진 변수(`CL`·`_CL_`·`LINK`·`_LINK_`·`CCC_OVERRIDE_OPTIONS`·`COMPILER_PATH`·`CPATH`·`C_INCLUDE_PATH`·`CPLUS_INCLUDE_PATH`·`OBJC_INCLUDE_PATH`·`LIBRARY_PATH`)는 목록 밖이라 빠지며(실측: `CL=/fp:fast`가 clang-cl에 fast-math를 켠다), 있었으면 `NativeToolchain.RemovedEnvironment`에 남고 빌드 CLI가 `Build environment: removed …` 한 줄로 알린다. 기본 config 파일(Homebrew LLVM의 `etc/clang/*.cfg`)은 discovery가 받는다고 확인한 컴파일러면 모든 드라이버의 CompileArguments·LinkArguments·probe 맨 앞의 `--no-default-config`(접두 없음 — clang-cl도 직접 받는다)로 끈다. 받지 않는 컴파일러가 config 파일을 읽으면(`--version`의 `Configuration file:` 줄) discovery 오류다. MetaCompiler는 이 인자를 허용 목록에만 두고 네이티브 프로필이 실어 올 때 쓴다.

모듈의 `AdditionalCompileArguments`·`AdditionalLinkArguments`는 FP 모델 인자(`FloatingPointArguments.IsModelArgument` — `-ffast-math`·`/fp:*`·`-ffp-*`·ISA 선택 `-m*` 등), `-mllvm`·`/mllvm:`·`-Wl,-mllvm,`·`-Xlinker`·`-Ofast`, CL 옵션(`/`·`-` 두 접두를 대소문자 구분으로 대조 — `-Fo`·`-EHsc`도 잡고 `-fdiagnostics-color`는 오인하지 않는다), 절대 경로(`/LIBPATH:`·`-L`·`-Wl,-rpath,`·`-fmacro-prefix-map=`·`-iquote`, 경로를 담은 define 값)를 거부한다. 외부 파일은 include 디렉터리·`AdditionalInputs`·`PrebuiltLibraries`로 선언한다.

## 빌드 주입 상수(BuildInfo)

`NativeModuleInput.ProvidesBuildInfo` 표지 모듈(엔진 Core — `Core/Build/BuildInfo.h`)에 닿는 `Executable` 모듈마다 빌드가 출력 잠금 아래에서 `<out>/BuildInfo/<Module>.BuildInfo.cpp`(`rocks::build::ToolchainFingerprint()`·`ToolBuildId()` 정의)를 만들어 그 모듈 소스에 더한다(계획 두 단계 — 1단계 계획으로 manifest를 만든 뒤 액션 그래프를 세운다). 같은 폴더에 `<Module>.BuildInfo.txt`(manifest 원문, SHA-256 = ID)와 `ToolchainFingerprint.txt`(지문 정규 텍스트, SHA-256 = 지문)가 있다. 이름 "Core"로 판정하지 않는다.

- 툴체인 지문(`ToolchainFingerprint.CanonicalText`·`Compute`): 컴파일러 식별·버전, 컴파일러·링커 바이너리 SHA-256, 대상(MSVC 접미 제거), 빌드 프로필의 FP 모델 인자. 구성·호스트 경로·환경은 넣지 않으며 모든 구성 × 최적화 × 언어 조합의 FP 모델 인자가 같아야 한다. 에셋 너겟 키에만 쓴다.
- 도구 빌드 ID: 그 실행 파일 도달 집합의 Compile·Archive·Link 액션(정규화한 인자 — 툴체인 인자 부분열 제외, object는 `obj:<소스 키>`, 경로는 키)과 입력(소스, 선언 입력, include 디렉터리 inventory, prebuilt — 액션 출력·`<out>/BuildInfo`·`OwnedRoots` 제외, MetaCompiler 생성 헤더 포함)의 내용 SHA-256. 경로 키는 `Path.GetRelativePath`로 가장 긴 루트부터 `out:`·`sdk:`(설치 SDK 게임 빌드의 `SdkDirectory`)·`src:`이고, 루트 밖 include는 `ext<i>:`, 그 밖 파일은 `ext:<이름>`이며 그때 `host-dependent: yes`다. 텍스트 확장자(header·`.cpp .cc .cxx .c++ .m .mm`)만 CRLF를 LF로 정규화해 해시한다. 진단·캐시 무효화용이며 패키지에 쓰지 않는다.
- 증분: 생성 TU의 key는 cpp 내용이고 `<out>/BuildInfo`는 header 디렉터리가 아니다. ID가 같으면 생성 파일을 다시 쓰지 않아 no-op이 0 액션이고, Core에 닿는 실행 파일은 입력 byte가 하나만 바뀌어도(주석 포함) 생성 TU와 link가 다시 돈다. 첫 InputSnapshot은 Prepare가 읽은 digest·열거의 메모로 만들어 마지막 비교가 Prepare 읽기부터 빌드 끝까지를 덮는다.
- `NativeBuildResult.ToolchainFingerprint`(대상이 없으면 null)·`ToolBuildIds`(link가 성공한 실행 파일만)는 action-report.json에 실린다.

## 입력과 증분 계약

source list/content, 유효 compile/link 옵션, 의존 artifact, public/private header inventory와 내용, 명시 AdditionalInputs, toolchain과 ContextFingerprint를 key에 포함한다. 확장자가 `.h/.hh/.hpp/.hxx/.inl/.inc/.ipp/.tpp`인 header와 source 옆 local header를 보수적으로 추적한다. 다른 확장자의 include/추가 생성 입력은 AdditionalInputs로 선언해야 한다. 실제 depfile 기반 정밀 의존 축소는 후속 작업이다.

repository root를 include해도 `out`과 현재 output의 임시 산출물은 탐색하지 않는다. `out/Generated`처럼 출력 tree 내부를 직접 include root로 선언한 경우 그 root는 별도 입력으로 검사한다. include 디렉터리 inventory(headersOnly 열거)는 점으로 시작하는 하위 디렉터리(`.claude`·`.vs`·`.native-tmp` 등), `.git`(파일 또는 디렉터리)을 가진 중첩 작업 사본, `.native-build.lock`을 가진 Rocks 출력 디렉터리를 깊이와 무관하게 뺀다(열거 루트 자신은 빼지 않는다). 새 optional/shadow header의 생성·삭제도 inventory digest를 바꾸므로 기존 depfile의 positive include만 믿지 않는다.

cache hit은 key와 모든 출력 파일의 실제 SHA-256이 일치할 때만 인정한다. 입력이나 출력이 없거나 변조되면 재실행한다. compiler 실패 시 기존 출력은 유지하고 임시 산출물을 제거한다. dependency는 모든 출력 게시 이후에만 실행되며, crash 중 부분 교체가 남아도 다음 cache 검증은 그 상태를 성공으로 재사용하지 않는다. 게시 중 일반 파일 오류는 이전 출력을 복구한다.

한 output directory의 writer는 프로세스 간 파일 잠금으로 직렬화한다. 다른 directory는 독립 실행한다. 실행 슬롯을 얻기 전에 dependency를 기다리므로 작은 worker 예산에서도 DAG가 진척한다. 취소는 공통 ProcessRunner로 compiler/linker 자식 트리를 종료하고 join한 뒤 임시 출력과 잠금을 반환한다. source/toolchain 변경을 실행 전후에 검출한 결과는 성공이 아니며 이번 action cache를 폐기한다.

caller는 요청 collection과 소스 tree를 진행 중 임의 변경하지 않아야 한다. 별도 프로세스가 같은 output을 직접 덮어쓰는 동작은 지원하지 않는다. SDK 이식성 검사는 상위 manifest 검증의 책임이며, 이 실행기의 절대 경로를 포함한 보수적 local key를 portable SDK 신원으로 사용하지 않는다.

## 검증

`dotnet run --project Programs/Rocks.Build.Tests/Rocks.Build.Tests.csproj`는 별도의 작은 C++ library/game fixture를 실제 compiler로 빌드한다. no-op 실행 0, game 변경의 engine action 0, private/public header 및 inventory 변경, SDK/context 변경, 출력 변조/삭제, compile 실패·재시도, 동일 output 경쟁, cycle/잘못된 입력, root include/Generated 경계, Debug/Release/ASan 실행, 취소와 자식 종료를 검증한다. `BuildInfoTests`는 지문 golden·조합 동일·버전 줄 괄호 규칙, 실제 discovery 신원, 환경 위생(`CL=/fp:fast` 대조), FP·CL·절대 경로 인자 거부, 줄 끝 정규화(버퍼 경계 CR), 가짜 Core ← Engine ← Game 주입(출력 digest = C# 계산), 표지 없는 Core, 증분(no-op 0, Engine 편집은 Engine·생성 TU·link만, CRLF 전환은 ID 불변), 호스트 무관(두 루트·다른 출력 이름·subst·8.3·junction), 구성 사이 지문 동일·ID 다름, `ext` 키, inventory 제외, 실패한 link, 빈 신원 거부를 검증한다. 실패 fixture는 경로를 출력하고 보존한다.

Windows 실제 회귀 31그룹(BuildInfo 9묶음 포함 — 허용 목록 환경·우회 인자·경로 경계 포함)은 통과했다. macOS 실행은 별도 native CI 결과를 확인하기 전에는 통과로 기록하지 않는다. 기존 Foundation 179개와 새 CLI 전체 경로의 통합 검증은 상위 빌드 담당자가 수행한다.
