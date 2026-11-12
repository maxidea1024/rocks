# Rocks.Build native 실행기

`NativeBuildEngine.BuildAsync`는 명시한 모듈 그래프에서 C++23 compile → static archive → executable link action을 만들고 실행한다. CMake와 Python을 호출하지 않는다. 호스트 도구·C# 규칙 컴파일, Clang metadata 생성과 SDK/game 조립은 상위 CLI가 담당한다. `NativeCompileProfile.Arguments`가 native compile과 AST 생성의 공통 target 옵션이다.

## 현재 지원 범위

- Windows clang-cl/x64와 macOS clang++의 Debug/Release/ASan. Windows ASan은 선택한 x64 compiler runtime DLL 및 같은 디렉터리의 import/thunk를 함께 사용한다. Mac deployment target은 `MACOSX_DEPLOYMENT_TARGET` 또는 명시 기본 13.0이며 compile/link/ABI 신원에 동일하게 적용한다.
- 모듈 dependency는 public include/define을 전파한다. private include/define은 owner에서만 사용한다. 현재 dependency 자체의 public/private 구분은 없다. 정적 의존의 PrebuiltLibraries/AdditionalLinkArguments는 최종 link로 전달한다.
- 소스가 없는 static module은 header-only다. 가짜 archive를 만들지 않고 ModuleOutputs에도 기록하지 않는다. DLL/shared library, PCH, Unity, remote executor, artifact cache 서버는 아직 구현하지 않았다.
- `Identity`는 전체 compiler 진단용이며 설치 경로가 들어갈 수 있다. `AbiIdentity`는 Installed SDK용 compiler version/target·architecture·SDK version/build·deployment의 경로 독립 digest다. 실제 증분 key에는 별도로 compiler/archiver/linker 및 선언 SDK 경로의 내용과 실행 환경을 포함한다.

## 입력과 증분 계약

source list/content, 유효 compile/link 옵션, 의존 artifact, public/private header inventory와 내용, 명시 AdditionalInputs, toolchain과 ContextFingerprint를 key에 포함한다. 확장자가 `.h/.hh/.hpp/.hxx/.inl/.inc/.ipp/.tpp`인 header와 source 옆 local header를 보수적으로 추적한다. 다른 확장자의 include/추가 생성 입력은 AdditionalInputs로 선언해야 한다. 실제 depfile 기반 정밀 의존 축소는 후속 작업이다.

repository root를 include해도 `out`과 현재 output의 임시 산출물은 탐색하지 않는다. `out/Generated`처럼 출력 tree 내부를 직접 include root로 선언한 경우 그 root는 별도 입력으로 검사한다. `.native-tmp`는 입력 header 디렉터리가 아니다. 새 optional/shadow header의 생성·삭제도 inventory digest를 바꾸므로 기존 depfile의 positive include만 믿지 않는다.

cache hit은 key와 모든 출력 파일의 실제 SHA-256이 일치할 때만 인정한다. 입력이나 출력이 없거나 변조되면 재실행한다. compiler 실패 시 기존 출력은 유지하고 임시 산출물을 제거한다. dependency는 모든 출력 게시 이후에만 실행되며, crash 중 부분 교체가 남아도 다음 cache 검증은 그 상태를 성공으로 재사용하지 않는다. 게시 중 일반 파일 오류는 이전 출력을 복구한다.

한 output directory의 writer는 프로세스 간 파일 잠금으로 직렬화한다. 다른 directory는 독립 실행한다. 실행 슬롯을 얻기 전에 dependency를 기다리므로 작은 worker 예산에서도 DAG가 진척한다. 취소는 공통 ProcessRunner로 compiler/linker 자식 트리를 종료하고 join한 뒤 임시 출력과 잠금을 반환한다. source/toolchain 변경을 실행 전후에 검출한 결과는 성공이 아니며 이번 action cache를 폐기한다.

caller는 요청 collection과 소스 tree를 진행 중 임의 변경하지 않아야 한다. 별도 프로세스가 같은 output을 직접 덮어쓰는 동작은 지원하지 않는다. SDK 이식성 검사는 상위 manifest 검증의 책임이며, 이 실행기의 절대 경로를 포함한 보수적 local key를 portable SDK 신원으로 사용하지 않는다.

## 검증

`dotnet run --project Programs/Rocks.Build.Tests/Rocks.Build.Tests.csproj`는 별도의 작은 C++ library/game fixture를 실제 compiler로 빌드한다. no-op 실행 0, game 변경의 engine action 0, private/public header 및 inventory 변경, SDK/context 변경, 출력 변조/삭제, compile 실패·재시도, 동일 output 경쟁, cycle/잘못된 입력, root include/Generated 경계, Debug/Release/ASan 실행, 취소와 자식 종료를 검증한다. 실패 fixture는 경로를 출력하고 보존한다.

Windows 실제 회귀 14그룹은 통과했다. macOS 실행은 별도 native CI 결과를 확인하기 전에는 통과로 기록하지 않는다. 기존 Foundation 179개와 새 CLI 전체 경로의 통합 검증은 상위 빌드 담당자가 수행한다.
