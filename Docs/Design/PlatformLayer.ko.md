# 플랫폼 계층 — 공통 구현과 플랫폼 선택

## 목적과 범위

Rocks의 파일, 경로, 스레드, 동기화, 메모리 및 운영체제 기능은 공통 계약과 플랫폼별 구현으로 분리한다. 공통 구현을 기본으로 두고 플랫폼이 필요한 기능을 구체화하거나 대체하는 구조를 적용한다. 사용자에게는 단순하고 일관된 공개 API를 제공하며 엔진의 별도 C++ 타입에 Unreal의 접두사를 사용하지 않는다. Windows와 macOS가 최초 대상이며 후속 플랫폼을 추가할 수 있다. 이 문서의 목표 구조는 현재 모든 기능이 구현되었거나 검증되었다는 뜻이 아니다.

## Unreal 기준과 확인 범위

UE 5.8 공개 문서에서 확인한 구조는 기능마다 다르다.

- 일반 경로 연산과 엔진·프로젝트 디렉터리는 [FPaths](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FPaths), 빌린 문자열 기반 경로 연산은 [FPathViews](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FPathViews)에 있다.
- 운영체제 기능의 일반 기반으로 [FGenericPlatformProcess](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FGenericPlatformProcess), [FGenericPlatformMemory](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FGenericPlatformMemory), [FGenericPlatformMisc](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FGenericPlatformMisc)를 확인하였다. [FPosixOSPlatformProcess](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FPosixOSPlatformProcess)는 Generic에서 파생되는 공통 계층으로, 공식 설명은 Linux와 Mac의 플랫폼 계층이 다를 수 있음을 명시한다. 모든 Generic 기능이 실제로 구현되어 있다는 뜻은 아니다.
- 파일 I/O는 [IPlatformFile](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/IPlatformFile), 그 물리 기반 [IPhysicalPlatformFile](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/IPhysicalPlatformFile), 그리고 파일 래퍼 체인을 관리하는 [FPlatformFileManager](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FPlatformFileManager)로 구분된다. 이 경계는 정적 플랫폼 유틸리티의 상속 구조와 동일하지 않다.

### UE 5.6 소스 확인

추가로 UE 5.6 branch commit `cdda65cecacd9a1278020925c357bf4cc0b17e8c`의 제한된 소스 사본을 확인하였다. 검토한 원본 경로는 `Core/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformMisc.h`, `GenericPlatformFile.h`, `Windows/WindowsPlatformMisc.h`, `Windows/WindowsPlatformFile.h`, `Apple/ApplePlatformFile.h`, `Mac/MacPlatformFile.h`, `Misc/Paths.h`, `Misc/PathViews.h`와 대응 `Paths.cpp`, `PathViews.cpp`다. 검토용 사본은 현재 작업 공간의 `work/ue5-reference/`에 있으며 엔진 저장소에 포함하지 않는다. 이는 UE 5.6의 해당 파일 확인 결과다. UE 5.8의 전체 플랫폼 구현이 같다는 증거는 아니며 실제 플랫폼 소스 전수 감사는 남아 있다.

- `FGenericPlatformMisc`는 `NormalizePath` 기본 구현 두 종류와 절대 경로 형식 판정 `IsValidAbsolutePathFormat`을 제공한다. 해당 5.6 소스의 기본 경로 보정은 아무 동작도 하지 않고, 기본 절대 형식 판정은 참을 반환한다. `FWindowsPlatformMisc`는 Generic에서 파생하고 절대 형식 판정을 별도로 선언한다. 실제 Windows 구현 내용은 확보한 헤더만으로 확인하지 않았다.
- `FPaths::NormalizeFilename`과 `FPathViews::NormalizeFilename`은 구분자 처리 후 선택된 `FPlatformMisc::NormalizePath`를 호출한다. Generic 주석에는 Unix의 홈 디렉터리 확장 예가 있지만, 확인한 사본에 해당 Unix 구현은 포함되지 않는다. 플랫폼 보정은 순수 문자열 정규화와 OS 정보 조회가 함께 필요할 수 있다.
- 파일은 `IPlatformFile`의 물리 파일 획득·하위 래퍼 연결 인터페이스, `IPhysicalPlatformFile`의 물리 기본 구현, Apple의 `FApplePlatformFile` 구체 구현이 분리되어 있다. 확인한 5.6 `MacPlatformFile.h`는 Apple 파일 헤더를 포함하고, 확인한 `WindowsPlatformFile.h`는 선언 없이 비어 있다. Windows 물리 파일 구현의 실제 정의 위치는 이 헤더만으로 확정하지 않는다. `FPaths::FileExists`는 `IFileManager`에 위임한다.
- Unreal의 해당 경로 정규화는 백슬래시를 슬래시로 바꾸고, 확장자 조회는 `.gitignore`를 `gitignore`로 해석한다. 현재 Rocks POSIX 문법의 백슬래시 보존 정책과 차이가 있다. Rocks의 확장자 조회는 현재 `.gitignore`에서 `gitignore`를 반환한다. 구조 참고와 동작 동일성은 별도로 검증한다.

로컬 `C:/work/uwo/engine`은 UE 4.27.2이며 UE5 구현의 기준으로 사용하지 않는다. `FPlatformPath`라는 별도 UE5 타입의 존재·구현은 확보한 범위에서 확인하지 못하였다.

## Rocks 목표 구조

```text
Base                          기본 자료형과 하위 공통 계약
  ↑
PlatformBase
  Public/Platform             호출자가 사용하는 공통 플랫폼 API
  Private/Generic             공통 기반 구현과 기본 동작
  Private/<플랫폼 계열>        공유할 수 있는 OS 계열 구현, 필요한 경우
  Private/Win64, Private/Mac   각 대상의 구현과 기능별 대체
  ↑
Core                          문자열·할당기·경로 문법·공통 엔진 유틸
  ↑
Engine / Tools / Game         프로젝트·설치 구성과 상위 시스템
```

위 디렉터리와 이름은 목표 구성이다. 현재 `Private/Common`을 사용하는 기존 파일을 한 번에 옮기는 것은 전환 전제나 완료 조건이 아니다. 기능별 Generic 타입은 공통 동작을 제공하고 플랫폼별 타입은 실제 차이가 있는 기능을 선택적으로 재정의한다. 호출자는 대상별 타입 이름을 직접 선택하지 않고 공통 이름을 사용한다. 컴파일 대상의 플랫폼 연결은 빌드에서 결정하며 호출부에 플랫폼 전처리 분기를 확산하지 않는다. Generic 기본 구현은 실제 계약을 충족하는 경우에만 사용한다. 필수 OS 기능에 빈 성공 구현을 제공하지 않는다.

타입 이름·선택 경계·표준 호출 형태는 사용자 접근성을 기준으로 유지한다. 기존 공개 API 변경이 필요한 경우 전환 대상과 ABI, 호출자, lifetime, 테스트를 함께 검토한다. Base/PlatformBase는 Core를 역참조하지 않는다. 플랫폼별 공유 계층은 실제로 공통 계약과 구현이 존재할 때만 도입한다.

## 파일과 경로

경로 문자열의 root·join·normalize·relative·이름 분리는 공통 Core 유틸리티에 둔다. Windows와 POSIX 문법은 실제 호스트와 독립적으로 선택할 수 있어야 하며 Windows 대상 콘텐츠를 macOS에서 처리할 수 있도록 두 문법을 함께 빌드한다. 백슬래시, 드라이브 상대 경로, UNC와 POSIX root의 규칙은 분리하여 검증한다. 문자열 정규화나 포함 관계는 심볼릭 링크·junction·실제 파일 동일성·접근 권한을 검증하지 않는다. 에셋 가상 경로와 OS 파일 경로도 구분한다.

플랫폼 경로 보정 및 절대 형식 판정은 위 Generic/플랫폼 계층의 별도 hook으로 설계한다. 기존 `Path::Normalize`는 입력으로 선택한 Windows/POSIX 문법의 순수 문자열 연산으로 유지하고, 실제 호스트 환경을 조회하거나 `~` 등을 적용하는 연산은 그 의존성과 실패를 드러내는 별도 경로로 제공한다. 필요하면 상위 편의 API가 문법 정규화 후 호스트 hook을 호출한다. hook을 모든 대상에 지원한다고 가정하지 않으며, 현재 Rocks에는 이 경로 보정 및 형식 검증 hook이 구현되어 있지 않다. Unreal 5.6의 Generic 기본 형식 판정이 항상 참인 사실을 Rocks의 검증 성공 보장으로 사용하지 않는다. 파일 존재·권한 검사와 실제 경로 접근은 hook과 별도다.

플랫폼 파일 I/O는 `Platform/File.h`의 `FileSystem`·`FileHandle` 인터페이스와 소유형 `File`, Windows/macOS 물리 backend로 초기 구현하였다. `File::Open(path, mode)`는 물리 파일 시스템을 사용하며 `File::Open(system, path, mode)`는 지정한 파일 시스템을 사용한다. 두 편의 overload의 경로는 NUL로 끝나는 UTF-8이며 첫 NUL 이후를 읽지 않는다. embedded NUL을 검증해야 하는 입력은 파일 시스템과 길이를 명시하는 overload를 사용한다. 파일 열기·닫기·부분 읽기/쓰기·seek·크기·파일 존재 여부·삭제를 제공한다. `Read`/`Write`는 한 호출의 부분 전송량과 오류를 함께 반환하며 읽기 0바이트 성공은 EOF다. `ReadExact`/`WriteAll`은 전체 요청 중 같은 `File`의 위치를 직렬화하고 EOF·진행 없음·부분 실패를 구분한다. 독립된 `File` 간의 파일 내용·위치 일관성은 보장하지 않는다. `FileSystem`의 직접 호출자는 핸들의 직렬화·닫기·소멸과 backend 수명을 관리한다.

현재 물리 구현은 UTF-8 범위의 유효성과 embedded NUL을 확인하고 Windows에서 UTF-16으로 변환한다. Windows 장치 namespace·확장 경로 및 레거시 DOS 장치 이름은 거부하지만 `FILE_TYPE_DISK` 검사만으로 모든 링크·재분석 대상이 일반 파일임을 보장하지 않는다. macOS 구현은 열린 대상의 일반 파일 여부를 검사한다. `Exists`의 결과는 이후 접근까지 유지되거나 보안 경계를 제공하지 않는다. `CreateAlways`는 이후 오류가 반환되어도 기존 파일이 잘렸을 수 있다. `Close` 실패 시 이전 native 핸들로 재시도하지 않으며 OS의 실제 자원 회수는 보장하지 않는다.

파일 시스템의 인터페이스는 wrapper 구현을 허용하지만, wrapper 체인 관리자와 `ReadAt`·`Flush`·`Truncate`·메모리 매핑 파일 기능은 아직 구현되지 않았다. 디렉터리·Stat·Copy/Move와 기본 경로는 아래 "디렉터리·파일 정보·기본 경로" 절을 따른다. wrapper의 구성·교체·동시 호출·종료, 부분 I/O와 OS 권한·오류의 규약을 후속 단계에서 검증한다. 기존 Windows 독립 테스트에서는 실제 심볼릭 링크 생성이 **SKIP**되었다. 이후 개발자 모드를 활성화하고 별도 재검증하여 유효한 파일 링크와 대상이 삭제된 링크의 판정을 확인했다. macOS 실기기와 실제 OS close 실패 주입도 미검증이다.

실행 파일·사용자·임시·캐시 디렉터리는 OS 플랫폼 backend가 조회하고, 엔진·프로젝트·콘텐츠·로그 경로는 명시적인 엔진/프로젝트 초기화 정보로 구성한다. OS 경로 조회와 파일 생성·접근 가능 여부 확인은 별도 연산이다. Windows UTF-16과 Core UTF-8 변환에는 유효성·길이·종단 문자·변환 실패를 명시한다. strict UTF-8로 표현할 수 없는 네이티브 이름에 대한 처리 정책은 후속 설계다. macOS sandbox 및 Windows 경로 namespace에 대한 검증도 필요하다.

## 현행 구현 감사

| 영역 | 현재 확인된 구현 | 한계와 전환 시 확인 |
|---|---|---|
| 경로 | `PathStyle::{Windows, Posix, Unsupported}`와 `GenericPlatformPath`의 미지원 기본값, `WindowsPlatformPath`·`ApplePlatformPath`의 선택, Core의 경로 문자열 처리 | 두 문법은 호스트와 독립적으로 선택한다. OS 환경에 의존하는 경로 보정·절대 형식 판정 hook 및 OS 디렉터리 조회는 후속이다. |
| 파일 | `FileSystem`·`FileHandle`·소유형 `File`, Windows/macOS 물리 backend와 부분/정확 전송의 초기 구현, 디렉터리·Stat·Copy/Move·재귀 helper·기본 경로(2026-11-11) | wrapper 관리자·`ReadAt`·`Truncate`·mapped·파일 감시는 후속이다. Windows 링크 실검사 SKIP, macOS 및 실제 OS close 오류 주입은 미검증이다. |
| 스레드 | `Private/Common/Thread.cpp`의 공통 수명·join 상태기계, 플랫폼별 `Concurrency.cpp`의 native thread 생성·join | 종료·TLS·자기 join·실패·동시 제어를 유지하고 각 OS에서 실행 검증한다. |
| 동기화 | `Private/Common/Sync.cpp`의 event·semaphore와 플랫폼별 mutex·condition variable | timeout·spurious wake·소유권·파괴 조건을 보존한다. RWLock 등은 별도 후속 항목이다. |
| 일반 메모리·시계·Fatal | 플랫폼별 `Platform.cpp`의 할당·mutex·시계·thread ID·치명 오류 경로 | 기능별 분리와 Generic/플랫폼 경계를 검토한다. OS 동작이 필요한 부분을 임의 Generic 성공으로 대체하지 않는다. |
| 가상 메모리 | 공통 `VirtualMemoryRegion`의 소유·범위·poison 상태와 플랫폼별 reserve/commit/decommit/release backend | Windows와 macOS의 메모리 보장 차이를 유지한다. OS 실패 주입과 macOS 실기기 검증은 후속이다. |
| 빌드 | `Foundation.Build.cs`가 Common과 지정된 플랫폼 소스를 선택한다. 현재 빌드 컨텍스트의 플랫폼 선택은 알려지지 않은 값을 명시적으로 거부한다. | 각 필수 기능의 구현이 정확히 하나 선택되는지, 신규 플랫폼과 target/architecture를 잘못 다른 플랫폼으로 처리하지 않는지 검증한다. |

## 이번 구현의 Windows 검증 결과

- Foundation Debug는 native 354개, 도구의 native/자식 프로세스 하네스 65개, 메타컴파일러 하네스 41개에서 실패가 없었다. Foundation AddressSanitizer는 native 354개, native/자식 하네스 66개, 메타컴파일러 하네스 41개에서 실패가 없었다. 각 실행의 build evidence에서 소스가 실행 중 변경되지 않았고 해당 runner 보고서가 최신임을 확인하였다. 실행 증거는 각각 `out/final-platform-foundation-debug/`와 `out/final-platform-foundation-asan/`이다.
- 두 native 테스트 실행에서 실제 Windows 심볼릭 링크 생성은 권한 또는 파일 시스템 제한으로 `SKIP`되었다. 집계된 테스트 수에 해당 항목이 포함되어도 링크 대상 판정의 실제 Windows 동작을 검증한 것으로 간주하지 않는다. macOS 빌드·실행과 실제 OS close 실패 주입은 이번에 수행하지 않았다. OS close 실패는 자원 회수 여부가 불확정인 한계를 유지한다.
- 빌드 규칙 테스트는 Windows·macOS 디렉터리 선택, 정의되지 않은 플랫폼의 오류 및 현재 미지원인 호스트/타깃 교차 빌드 거부를 확인한다. 별도의 Apple 플랫폼 private 헤더 증분 fixture는 Windows 호스트에서 Mac 형태의 의존성 규칙을 평가해 헤더 변경 시 재컴파일·재링크와 무변경 시 no-op을 검증한다. 이 fixture는 macOS SDK 컴파일이나 macOS 실행 검증이 아니다.

## 디렉터리·파일 정보·기본 경로 (2026-11-11 1차 구현)

`FileSystem`에 `Stat`·`MakeDirectory`·`DeleteDirectory`·`EnumerateDirectory`·`Move`·`Copy`를 가상 함수로 추가했다. 기본 구현은 `Unsupported`이므로 기존 wrapper는 그대로 컴파일되고 지원하는 연산만 재정의한다. 이름은 Windows SDK 매크로(`CreateDirectory`·`RemoveDirectory`·`CopyFile`·`MoveFile`)와 겹치지 않게 정했다(UE는 매크로를 숨기는 방식을 쓴다).

| 연산 | 계약 | Windows | POSIX(macOS 미빌드·Android) |
|---|---|---|---|
| `Stat` | link를 따라간 대상의 종류·크기·수정 시각, `isLink`는 경로 자체가 link인지. 시각은 0001-01-01 UTC 기준 100ns tick이며 9999-12-31 이후·0001-01-01 이전은 0. 대상 없는 link는 NotFound, 끝 구분자는 OS 규칙 | `OPEN_REPARSE_POINT` handle의 `FileAttributeTagInfo`(이름 대체 reparse tag만 link) + backup semantics handle의 `GetFileInformationByHandle` | `lstat` + `stat`, `st_mtimespec`/`st_mtim` |
| `MakeDirectory` | 한 단계, 이미 있으면 AlreadyExists | `CreateDirectoryW` | `mkdir(0700)` |
| `DeleteDirectory` | 빈 디렉터리만, 비었지 않으면 DirectoryNotEmpty, 파일이면 WrongType, 디렉터리 link는 link만 | `RemoveDirectoryW` | `lstat` 후 `unlink`/`rmdir` |
| `EnumerateDirectory` | 직접 자식, "."·".." 제외, 순서 없음, link를 따라가지 않은 항목 정보, visitor가 false면 중단 | `FindFirstFileExW`(large fetch) | `opendir`/`readdir` + `fstatat(AT_SYMLINK_NOFOLLOW)` |
| `Move` | 같은 volume 안 rename, replace 여부 선택, 디렉터리는 기존 대상을 덮어쓰지 않음(AlreadyExists), 파일·디렉터리 혼합은 WrongType(link는 대상과 무관하게 디렉터리 아님), 같은 파일 객체는 OS에 맡김(대소문자만 변경은 모두 성공, hard link 사이는 Windows가 원본 이름 제거·POSIX는 변화 없음), 다른 volume은 Unsupported | 속성·link·128-bit file id(`FILE_ID_INFO`) 사전 검사 + `MoveFileExW`(COPY_ALLOWED 없음) | `lstat`(dev·ino) 사전 검사 + `rename` / `renamex_np(RENAME_EXCL)`·`renameat2(RENAME_NOREPLACE)` |
| `Copy` | 일반 파일 하나, replace 여부 선택, 실패 시 대상 제거(replace로 덮어쓰던 기존 대상도 없어짐), 같은 파일 객체(link를 거친 경우 포함)로의 복사는 InvalidArgument | link를 따라간 file id 비교 + `CopyFileW` | dev·ino 비교 + read/write 반복, 실패 시 `unlink` |

`Delete`는 파일과 link 자체(대상이 없어도)를 지우며 실제 디렉터리는 WrongType이다. Windows에서 link가 아닌 reparse point(OneDrive placeholder 등)가 붙은 디렉터리도 WrongType이다. 공용 helper `MakeDirectoryTree`는 루트·드라이브·UNC 공유 이름을 건너뛰고 부모부터 만들며 중간에 파일이 있으면 WrongType이다. `DeleteDirectoryTree`는 디렉터리마다 자식 목록을 먼저 모은 뒤 지우고, link는 따라가지 않고 link만 지운다. 루트로 넘긴 경로의 끝 구분자는 떼어내므로 `link/`도 link만 지우며, 대상 없는 link 루트도 지운다. 마지막 구성 요소가 `.`·`..`이면 InvalidArgument다(POSIX는 `link/.`를 link 대상으로 해석하고 Windows는 문자열로 `link`로 정규화하므로 지울 대상이 달라진다). 순환 link나 Windows에서 디렉터리를 가리키는 파일 link처럼 OS가 따라갈 수 없는 link를 루트로 넘기면 Stat 오류를 그대로 돌려주고 아무것도 지우지 않는다(트리 안에 있으면 link만 지운다). 중간 실패는 그 상태로 멈춘다. Windows 드라이브 상대 경로(`C:`)는 그 드라이브의 현재 디렉터리로 다룬다(열거 패턴 `C:*`, 자식 경로 `C:name`).

`GetStandardDirectory`(`Platform/Path.h`)는 실행 파일·작업·임시·홈·사용자 데이터·캐시 디렉터리를 UTF-8로 돌려준다. Windows는 `GetModuleFileNameW`·`GetCurrentDirectoryW`·`GetTempPathW`·`SHGetKnownFolderPath`(Profile·RoamingAppData·LocalAppData), macOS는 `_NSGetExecutablePath`+`realpath`·`getcwd`·`confstr(_CS_DARWIN_USER_TEMP_DIR)`·`HOME`(없으면 사용자 DB)·`~/Library/Application Support`·`~/Library/Caches`다. 버퍼가 부족하면 Overflow와 필요한 길이를 돌려준다.

검증: Windows x64와 Android x86_64 에뮬레이터에서 생성·중복·Stat 크기와 시각 범위·열거와 조기 중단·종류 오류·비어 있지 않은 디렉터리·Copy/Move 덮어쓰기 규칙(자기 자신 복사, 디렉터리 덮어쓰기 거부, 파일·디렉터리 혼합, 대소문자만 바꾸는 이름 변경)·재귀 생성/삭제·디렉터리 symbolic link를 따라가지 않는 재귀 삭제(끝 구분자 붙은 link 루트, 대상 없는 link 루트, 디렉터리를 가리키는 파일 link 포함, Windows는 개발자 모드에서 실제 link 생성)·Windows `C:` 열거·기본 경로 6종과 버퍼 부족·wrapper 기본값을 확인했다. macOS 구현은 빌드·실행하지 않았다. 읽기 전용 파일이 있는 트리의 강제 삭제, 시각·권한 변경, 파일 감시, 다른 volume 이동의 복사 fallback, iOS·Android 샌드박스 경로는 후속이다.

## 비동기 파일 읽기·쓰기 (2026-11-11 1차 구현)

`Platform/AsyncFile.h`는 텍스처·메시 스트리밍처럼 많은 offset 지정 읽기를 동시에 띄우고 완료를 비동기로 받는 저수준 API다(UE `IAsyncReadFileHandle::ReadRequest`·`IAsyncReadRequest` 계층 대응, UE의 IoDispatcher·패키지 로더는 상위 계층). 공용 수명·동기화 규칙은 `Private/Common/AsyncFile.cpp`, OS 호출은 `Private/<플랫폼>/AsyncFile.cpp`의 hook이 담당한다.

| backend | 플랫폼 | 방식 | 취소 |
|---|---|---|---|
| Native | Windows | `FILE_FLAG_OVERLAPPED` + IO completion port + 완료 스레드 1개. 즉시 실패·0 byte 요청도 completion packet으로 같은 스레드에 통지 | `CancelIoEx` 요청, 완료와 경합하면 실제 결과 보고 |
| WorkerPool | 모든 플랫폼(macOS 기본, 향후 iOS·Android) | worker 스레드가 위치 지정 동기 읽기·쓰기(Windows `ReadFile`/`WriteFile`+OVERLAPPED offset, POSIX `pread`/`pwrite`) | 대기열의 요청만 취소, 실행 중 I/O는 그대로 완료 |

쓰기(`AsyncWriteFile`): `FileOpenMode`의 `ReadWriteExisting`·`CreateNew`·`CreateAlways`로 열고 `SubmitWrite(request, offset, source, bytes)`로 서로 다른 offset에 동시에 쓴다. 쓰기 성공은 요청한 byte를 모두 쓴 경우뿐이며, 중간 실패는 실패 status와 그때까지 쓴 byte(진행 없음은 `NoProgress`)를 보고한다. 파일 끝 너머 쓰기는 OS 의미대로 0으로 채워 늘린다. `Flush`는 완료된 쓰기를 장치까지 내리는 동기 호출(Windows `FlushFileBuffers`, macOS `F_FULLFSYNC` 실패 시 `fsync`)이며 진행 중 요청이 있으면 Busy다. 요청 레코드는 읽기·쓰기 공용 `AsyncIoRequest`(`AsyncReadRequest`는 같은 타입의 별칭)다. Native 쓰기가 일부만 끝난 성공을 보고하면 실패로 바꿔 드러낸다.

수명 규칙: 요청 레코드(OVERLAPPED 포함)와 버퍼는 최종 완료까지 유지된다. 진행 중 요청의 소멸자는 취소 후 완료를 기다리고, 파일 소멸자는 그 파일의 모든 요청을 취소·대기한 뒤 닫는다. `Close`는 진행 중 요청이 있으면 Busy, context `Shutdown`은 열린 파일이 있으면 Busy, 열린 파일을 남긴 context 소멸은 Fatal이다. 완료 스레드는 mutex 안에서 결과를 게시한 뒤 요청·파일에 다시 접근하지 않으며, callback은 게시 뒤 요청 대신 사용자 context만 받는다.

검증: Windows x64에서 두 backend로 동시 32개 읽기·EOF·짧은 읽기·0 byte·잘못된 제출·Busy·대량 취소 경합·소멸자 대기, 그리고 동시 쓰기 8개의 역순 제출 후 읽기 대조·열기 모드 3종·끝 너머 확장의 0 채움·0 byte 쓰기·소멸자 대기를 실제 파일로 확인했다(비동기 파일 테스트 10건 100회 반복 실패 0). macOS backend(pwrite·`F_FULLFSYNC` 포함)는 빌드·실행하지 않았다. 디스크 가득 참 같은 쓰기 실패 주입, scatter/gather, 우선순위·deadline, dispatch I/O 평가, 결정적 fake backend interleaving, Android asset·iOS 번들 경로는 후속이다.

## 자식 프로세스와 pipe (2026-11-13 1차 구현)

`Platform/Process.h`는 자식 프로세스의 시작·대기·종료 코드·강제 종료와 표준 입출력 pipe를 제공한다(UE `FGenericPlatformProcess`의 `CreateProc`·`IsProcRunning`·`WaitForProc`·`TerminateProc`·`GetProcReturnCode`·`CreatePipe`·`ReadPipe`·`WritePipe`·`ExecProcess`·`IsApplicationRunning` 대응). 셰이더 컴파일 워커와 외부 도구(Apple `metal` 등) 실행의 선행 기반이다. 공용 입력 검사는 `Private/Common/Process.cpp`, OS 호출은 `Private/<플랫폼>/Process.cpp`가 맡는다. Windows UTF-16 변환·실행 파일 경로·오류 변환은 파일 backend와 같은 `Private/Win64/WindowsString.h`·`WindowsError.h`를 쓴다.

| API | 계약 |
|---|---|
| `Process::Start(ProcessStartInfo)` | 실행 파일(UTF-8, PATH 검색 없음, 상대 경로는 부모 현재 디렉터리 기준, 자식의 argv[0]은 받은 문자열 그대로), 인자 배열(byte 그대로 전달), 작업 디렉터리, 표준 스트림 셋의 `Null`·`Inherit`·`Pipe`, 오류의 출력 합치기, 우선순위(`Normal`·`Low`). 실패하면 만든 자원을 모두 정리한다. 작업 디렉터리 없음은 NotFound, 디렉터리 아님은 WrongType, 명령줄이 OS 한도를 넘으면 Overflow(Windows 32767 UTF-16 단위, POSIX `E2BIG`)다. Windows batch 파일(`.bat`·`.cmd`)은 cmd.exe가 인자를 다시 해석하므로(인자 주입) Unsupported, 실행 파일 경로의 `"`는 InvalidArgument다 |
| `Wait(timeout)`·`GetExit` | 직접 자식의 끝남을 기다린다(`Signaled`·`TimedOut`·`Failed`). 종료는 `Exited`(코드), `Signaled`(POSIX 신호), `Terminated`(부모가 요청). OS 대기 자체가 실패하면 `TimedOut`이 아니라 `Failed`다 |
| `Terminate` | 자식과 자손을 강제 종료한다. 기다리지 않는다. 먼저 자식이 이미 끝났는지 확인해 그렇다면 종료 종류(`Exited`·`Signaled`)를 그대로 두고 남은 자손만 끝낸다. 성공해야 요청을 기록하므로 실패하면 다시 시도할 수 있다. 중복 호출은 성공이다 |
| `WriteInput`·`CloseInput`·`Read` | 시간 제한 있는 pipe 입출력이다. 시간 초과는 `NoProgress`이며 `WriteInput`이 돌려주는 바이트 수는 실제로 자식 pipe에 들어간 양과 정확히 같다. 모든 쓰는 쪽이 닫힘은 `EndOfFile`, Pipe가 아니면 `NotOpen`이다 |
| `Close`·소멸 | 살아 있는 자식·자손을 끝내고 회수한 뒤 모든 handle을 닫는다. 직접 자식이 먼저 끝났어도 남은 자손을 끝낸다. 끝나기를 최대 10초 기다리고, 끝나지 않으면(커널 대기, 종료 중 교착) handle만 놓는다. 분리 실행(detach)은 없다 |
| `RunProcess` | 입력을 넣고 출력·오류를 한 스레드에서 동시에 모으며(용량을 넘으면 버리면서 끝까지 읽음) 끝날 때까지 기다린다. 시간 초과면 강제 종료한다. 직접 자식이 끝나거나 강제 종료하면 pipe를 잡은 남은 자손을 끝내고 남은 출력을 최대 10초 더 모은다. job·그룹 밖 프로세스가 pipe를 잡고 있으면 그 뒤를 버리고 `*Truncated`를 켠다. 그래서 전체 시간은 timeout + 10초를 넘지 않는다. 한 pipe가 쉬지 않고 출력해도 다른 pipe·입력·시간 초과 확인이 굶지 않는다 |
| `CurrentProcessId`·`IsProcessRunning`·`GetExecutablePath` | 워커가 부모 PID로 부모 생존을 확인하는 용도와 자기 실행 파일 경로다. `IsProcessRunning`은 Windows에서 끝난 프로세스를 false로, POSIX에서 회수 전 zombie를 true로 본다. PID 재사용 때문에 부모 감시는 입력 pipe EOF(lifeline)를 먼저 쓴다 |

| 플랫폼 | 구현 |
|---|---|
| Windows | 자식을 `CREATE_SUSPENDED`로 만들어 Process 전용 job object(`KILL_ON_JOB_CLOSE`·`DIE_ON_UNHANDLED_EXCEPTION`, 탈출 불가)에 넣은 뒤 실행한다. 부모가 비정상 종료해도 job handle이 닫히며 자식·자손이 끝나고, 처리되지 않은 예외는 오류 대화상자 없이 끝난다. 상속 handle은 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`로 표준 스트림 셋만 넘긴다. `Inherit`는 부모 표준 handle이 실제로 열린 handle일 때만 물려주고(`GetFileType` 확인) 아니면 NUL이다. `CREATE_NO_WINDOW`. pipe는 named pipe(원격 접속 거부, 첫 인스턴스만, 이름에 PID·순번·난수)다. 부모가 읽는 출력·오류 쪽은 overlapped라 시간 제한 읽기와 `WaitForMultipleObjects` 동시 대기가 된다. 부모가 쓰는 입력 쪽은 비차단(`PIPE_NOWAIT`) 동기 handle이다. overlapped 쓰기를 시간 초과로 취소하면 자식이 이미 읽은 일부가 0바이트로 보고되기 때문이다(가득 차면 최대 10 ms 간격으로 다시 쓴다). 명령줄은 MSVC CRT·`CommandLineToArgvW` 규칙으로 인용한다. 작업 디렉터리는 `CreateProcessW` 제약으로 MAX_PATH 미만이어야 한다. UTF-16 변환·`\\?\` 접두사 제거·오류 변환은 파일 backend와 같은 `Private/Win64/WindowsString.h`·`WindowsError.h`를 쓴다 |
| macOS | `posix_spawn` + 새 프로세스 그룹 + `POSIX_SPAWN_CLOEXEC_DEFAULT`(file action으로 연결한 fd만 물려줌) + 신호 마스크·처리 초기화, `posix_spawn_file_actions_addchdir_np`. 종료는 `waitid(WNOWAIT)`로 회수 없이 관측하고(시간 제한은 kqueue `EVFILT_PROC`, 이벤트·`ESRCH` 뒤 다시 `waitid`로 확인), `Close`에서 그룹을 끝낸 뒤 회수한다. 회수 전에는 PID·그룹 번호가 재사용되지 않으므로 다른 그룹을 죽이지 않는다. 다른 코드가 자식을 회수하면(`ECHILD`) 종료 코드를 0xFFFFFFFF로 두고 이후 신호를 보내지 않는다. 입력 pipe는 `F_SETNOSIGPIPE`로 SIGPIPE 대신 EPIPE다. `RunProcess`는 `poll`로 pipe를 함께 기다리고 50 ms마다 직접 자식의 종료를 확인한다 |
| Android | `Start`·`RunProcess`는 `Unsupported`다(`posix_spawn`이 API 28부터이며 앱에서 자식 프로세스가 필요 없다). PID·생존 확인·실행 파일 경로만 제공한다 |

UE 5.6 원본(commit `cdda65ce`, 경로는 `Engine/Source/Runtime/Core/Private/` 기준)과 다르게 정한 점:

| 항목 | UE 5.6 | Rocks |
|---|---|---|
| 인자 | `CreateProc(URL, Parms)`가 `"\"URL\" Parms"` 문자열을 그대로 넘긴다(`Windows/WindowsPlatformProcess.cpp:470–561`) | 인자 배열을 받아 플랫폼이 인용한다. 호출자가 인용 규칙을 몰라도 된다. 인용으로 막을 수 없는 batch 파일은 거부한다 |
| 상속 handle | `CreateProc`은 상속 가능한 handle을 모두 넘긴다(`:503–531`). `ExecProcess`만 handle 목록으로 제한한다(`:962–999`) | 항상 표준 스트림 셋으로 제한한다 |
| 자손 종료 | Windows `TerminateProc(KillTree)`는 Toolhelp 스냅샷 재귀(`:614–681`), Mac은 직계 자식만 SIGTERM(`Mac/MacPlatformProcess.cpp:853–881`), Linux는 STUBBED | Windows job object, macOS 프로세스 그룹 SIGKILL. 직접 자식이 스스로 끝난 뒤에도 남은 자손을 끝낸다 |
| 부모 사망 | `ExecProcess(bShouldEndWithParentProcess)`만 전역 job에 `KILL_ON_JOB_CLOSE`를 건다(`:941–960`). ShaderCompileWorker는 부모 PID를 스스로 확인한다 | Windows는 모든 Process가 job으로 함께 끝난다. 워커의 부모 감시는 입력 pipe EOF를 먼저 쓰고 `IsProcessRunning(부모 PID)`는 보조다 |
| 작업 디렉터리 | Linux `CreateProc`은 받지만 쓰지 않는다(`Unix/UnixPlatformProcess.cpp`) | 모든 지원 플랫폼에서 적용하고 없음·디렉터리 아님을 구분한다 |
| pipe 읽기 | `ReadPipe`는 `PeekNamedPipe` 후 있는 만큼만 읽는다(`:1849–1871`). Metal 셰이더 경로는 프로세스가 끝난 뒤에야 읽는다(`Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:1081–1120`) | 시간 제한 `Read`와 출력·오류를 동시에 비우는 `RunProcess`. 출력이 pipe 버퍼보다 커도 멈추지 않는다 |
| PID 재사용 | Unix 구현 주석에 PID 재사용으로 워커 감시가 끝나지 않은 사례가 남아 있다(`Unix/UnixPlatformProcess.cpp:1184–1191`) | macOS는 회수 전에만 그룹을 끝낸다. `IsProcessRunning`은 같은 한계를 문서화하고 lifeline을 먼저 쓴다 |
| 우선순위 | `PriorityModifier` -2…2. ShaderCompileWorker는 -1(Windows `BELOW_NORMAL`, Unix nice +5) | `Normal`·`Low`(UE -1과 같은 값) |

한계:

- 자손으로 공유 서버를 띄우는 도구(MSVC cl의 `mspdbsrv`, sccache 서버, 빌드 데몬)는 `Close`·`RunProcess`가 자손을 끝내므로 매 실행마다 서버를 잃는다. 같은 서버를 쓰던 다른 실행이 실패할 수 있다.
- POSIX에서 그룹을 벗어난 자손(setsid·setpgid)과, 자식이 SIGKILL로 끝났을 때 자식이 자기 Process로 만든 손자(자기 그룹을 가진다)는 남는다(Windows는 중첩 job으로 함께 끝난다). POSIX에서 부모가 비정상 종료하면 자식은 남는다.
- macOS에서 같은 프로세스의 다른 코드가 자식을 회수하면(SIGCHLD `SIG_IGN`, `waitpid(-1)`) 종료 코드를 잃는다. 입력을 터미널에서 `Inherit`하면 자식이 백그라운드 그룹이라 읽는 순간 SIGTTIN으로 멈춘다.
- 같은 시각 다른 스레드가 handle 목록 없이 자식을 만들면(CreateProcess `bInheritHandles`, fork) 그 자식이 pipe 끝을 물려받아 EOF가 늦어질 수 있다.
- 한 Process는 한 스레드에서만 호출한다. Windows 작업 디렉터리는 MAX_PATH 미만이다.

후속(필요한 쪽이 생기면 순서대로): 환경 변수 지정, 분리 실행(detach)·(PID, 시작 시각) 정체·권한 상승 여부(Rockpile R0 자동 실행, [Rockpile.ko.md](Rockpile.ko.md) 15절), 다른 스레드에서의 취소, 정상 종료 요청(SIGTERM·CTRL_BREAK), 숨김 GUI 실행, PATH 검색, Android.

검증(2026-11-13): Windows x64 Debug와 Asan에서 프로세스 시험 21건과 Core 전체 시험 897건이 통과했다. 프로세스 시험은 종료 코드, 입력 검증 음성 대조(NUL·잘못된 UTF-8·`"`·`.BAT`·2 MiB 인자 Overflow·작업 디렉터리 NotFound·WrongType), 인용·UTF-8 인자 왕복(따옴표·역슬래시·빈 인자·줄바꿈·BMP 밖 문자), UTF-8 이름 실행 파일·작업 디렉터리, 상대 경로 실행 파일, 1 MiB 입출력 동시 진행, 출력·오류 동시 용량 초과와 용량 0, 합친 출력 순서, `Inherit`가 손자까지 이어지는지와 `Null`, 시간 초과 강제 종료(쉬지 않고 출력하는 자식 포함), 대기·종료·재시작·이동, 소멸 시 종료, 자손 종료, 자연 종료 뒤 남은 자손 종료, 손자가 pipe를 잡은 경우의 RunProcess 종료, 스트리밍 pipe와 EOF, 입력 시간 초과 때 쓴 양의 정확성과 읽는 쪽이 닫힌 입력, Low 우선순위, 부모 handle 비상속, 비정상 종료(신호·예외), PID·실행 파일 경로를 확인한다. 자식은 테스트 실행 파일을 `--process-probe`로 다시 띄운 probe다(`PlatformBase/Tests/ProcessProbe.cpp`, 플랫폼 도구는 `TestSupport/ProcessProbe.h`). Android x86_64 에뮬레이터(API 35)에서도 프로세스 시험 21건이 통과했다(Unsupported 경로와 PID·생존 확인·실행 파일 경로). 이 교차 빌드가 POSIX 시험 도구의 경고 오류를 잡아 macOS CI 전에 고쳤다. macOS 구현은 이 기계에서 빌드·실행하지 않았다(CI가 맡는다).

## 모바일 플랫폼 (2026-11-11: desktop + mobile은 우선 Android)

사용자 결정으로 mobile은 Android에 먼저 집중하고 iOS는 보류한다. PlatformBase는 다음 구조다.

| 디렉터리 | 내용 | 빌드 대상 |
|---|---|---|
| `Private/Common` | OS 호출 없는 공용 수명·동기화 규칙 | 모든 대상 |
| `Private/Win64` | Windows backend | Windows |
| `Private/Posix` | thread·mutex·조건 변수·clock·동기/비동기 파일·디렉터리·virtual memory의 POSIX 공통 구현, errno 변환(`PosixFileError.h`) | macOS·Android |
| `Private/Apple` | `PosixPlatform.h` hook의 Apple 구현(`st_mtimespec`, `renamex_np`, `F_FULLFSYNC`, `pthread_threadid_np`, 상대 시간 조건 변수 대기, `MADV_FREE`) | macOS |
| `Private/Mac` | 기본 경로(`_NSGetExecutablePath`·`~/Library`) | macOS |
| `Private/Android` | hook의 Android 구현(`st_mtim`, `renameat2(RENAME_NOREPLACE)` system call과 미지원 시 검사 후 rename, `fsync`, `gettid`, `CLOCK_MONOTONIC` 조건 변수, `MADV_DONTNEED`)과 경로(`/proc/self/exe`, 앱 등록 디렉터리) | Android |

POSIX 공통 코드는 hook 밖의 OS 전용 API를 직접 쓰지 않는다. policy 검사는 `Posix`·`Apple`·`Android`를 native backend 디렉터리로 인정한다. 테스트 지원 코드도 `Core/Source/Developer/TestSupport/Private/Posix`로 합쳤다.

Android 앱 전용 저장소 경로는 Java 쪽 Context에서만 알 수 있으므로 앱이 시작 시 `RegisterApplicationDirectories(data, cache)`로 넘긴다. 등록 전에는 `UserData`·`UserCache`가 NotFound이며, `Temporary`는 등록된 cache 또는 `TMPDIR`·`/data/local/tmp`, `Home`은 등록된 data 또는 `HOME`이다. desktop에서 등록은 Unsupported다.

검증(2026-11-11): NDK r29 clang(`--target=x86_64-linux-android26`, `aarch64-linux-android26`, `-Wall -Wextra -Wpedantic -Werror`)으로 Foundation 전체 166개 단위를 두 ABI에서 컴파일·링크했다. x86_64 바이너리를 Android 15(API 35) 에뮬레이터에서 실행해 662건 전부 통과했다(seed 4종, 비동기 파일 30회 반복 포함). 처음 실행에서 드러난 문제: POSIX 비동기 파일 오류 변환의 `EEXIST` 누락(macOS도 같은 코드라 같은 결함, 수정), 테스트 난수의 함수 인자 평가 순서가 ABI마다 달라 seed가 같아도 사례가 갈리던 테스트 결정성 결함(중괄호 초기화로 순서 고정), 기본 경로 테스트의 루트 경로·앱 등록 가정. arm64는 교차 컴파일만 했고 실기기 실행은 하지 않았다. 빌드는 Rocks.Build 정식 대상으로 통합했다(MB01).

```
# 빌드(Windows·macOS host, NDK는 --android-ndk 또는 ANDROID_NDK_HOME·SDK 기본 위치에서 찾음)
dotnet msbuild Rocks.proj -t:Build -p:RocksPlatform=android -p:AndroidAbi=arm64 -p:AndroidApi=26
# 기기·에뮬레이터에서 테스트(adb는 --adb 또는 ANDROID_HOME·SDK 기본 위치)
dotnet msbuild Rocks.proj -t:Test -p:RocksPlatform=android -p:AndroidAbi=x86_64
```

컴파일 인자는 desktop과 같은 경고·언어 규칙에 `--target=<abi>-linux-android<api>`·`-fPIC`·`-DROCKS_PLATFORM_ANDROID=1`을 더하고, 링크는 `-static-libstdc++`다(엔진은 STL을 쓰지 않지만 compiler runtime이 필요). 메타컴파일러는 같은 NDK clang으로 AST를 만들며 검토한 대상 인자만 허용한다. `test`는 실행 파일을 `/data/local/tmp/rocks-test`에 올려 실행하고 보고서를 회수하며, host에서 자식 프로세스를 띄우는 harness(Fatal 음성 대조)·self·meta 검사는 Android에서 아직 실행하지 않는다. Android ASan과 Installed SDK 내보내기는 거부한다. Windows host에서 x86_64는 빌드·에뮬레이터 테스트 662건 통과, arm64는 Release 빌드만 확인했다. macOS host 교차 빌드와 macOS 자체 빌드는 하지 않았다.

## 플랫폼 선택과 실패 정책

- 지원 대상, 호스트, 타깃, 아키텍처 및 필수 구현 목록을 빌드 규칙에서 명시한다. 알 수 없는 플랫폼은 타 플랫폼으로 대체하지 않고 진단과 함께 빌드를 중단한다. cross-compilation 지원 전에는 지원된다고 가정하지 않는다.
- Generic 또는 플랫폼 대체 구현의 선택은 기능별로 한 번만 이루어져야 한다. 누락·중복 정의, 단독 공개 헤더 컴파일, 타깃별 source inventory를 검사한다. optional 기능은 지원 여부와 실패 원인을 명시한다.
- 강제 오류·실패 주입은 지원하지 않는 성공값, 소유권 유실 또는 무한 대기로 전환되지 않아야 한다. opaque native storage의 크기·정렬 검사는 각 대상에서 실시한다. 경고는 오류다.
- GUI·렌더러 없이 headless 실행이 가능해야 한다. OS 리소스 사용과 thread affinity, 접근 권한, callback 재진입, shutdown 및 모듈 수명을 문서화한다.

## 전환 순서와 검증

1. 현재 빌드 선택의 실패 경로와 target 검증을 고정하고, 누락·중복 플랫폼 구현의 음성 대조를 추가한다. 경로 문법의 기존 Windows/POSIX 테스트를 두 호스트에서 모두 실행한다.
2. 공통/플랫폼 기능 경계와 선택 방식부터 정리한다. 경로 문자열의 두 문법을 공용 Core에서 유지하고 native 호스트 선택만 플랫폼 계층에서 담당한다. 플랫폼 hook의 문법·환경 의존과 미지원 표현을 별도 검토한다.
3. 기존 Thread/Sync/Memory/VirtualMemory를 기능 단위로 점진 전환하고 공개 사용법과 의미를 유지한다. 단위·병렬·실패·종료 테스트를 이전과 동일하게 실행한 뒤 다음 기능으로 이동한다.
4. 초기 파일 I/O의 부분·정확 전송, Unicode, OS 권한·오류·링크, 소유권과 실패 주입을 보강한다. 플랫폼 디렉터리 조회, 파일 wrapper 관리자와 디렉터리 순회·동기 나머지 기능, async/mapped는 단계적으로 추가한다.
5. Windows/macOS 각각에서 경고 오류 빌드, 공개 헤더 단독 컴파일, 정상·경계·음성 대조와 sanitizer 가능 범위를 확인한다. 실기기 및 검증되지 않은 플랫폼은 미검증으로 남긴다. 추가 플랫폼은 필수 구현과 공통 계약 테스트를 통과한 뒤 지원 대상으로 표시한다.

## UE 5.6 원본 기준 기반 기능 후속 목록

다음 항목은 UE 5.6 commit `cdda65cecacd9a1278020925c357bf4cc0b17e8c`의 지정 헤더 선언과 Rocks `Core/Source/Runtime/PlatformBase`, `Core/Source/Runtime/Core` 공개 API를 대조한 결과다. UE 헤더의 선언은 기능 범위의 근거이며, 해당 기능이 모든 UE 플랫폼에서 동일하게 구현된다는 뜻은 아니다. Rocks의 기존 일부 구현은 부분 지원으로 기록하고 나머지는 후속으로 분류한다.

| 우선순위 | 영역과 UE 5.6 확인 근거 | Rocks 현황과 후속 기능 | 검증 기준 |
|---|---|---|---|
| P0 | 메모리 기본 연산: `GenericPlatform/GenericPlatformMemory.h`의 `Memmove`, `Memset`, `Memzero`, `Memcpy`, `Memcmp`, `Memswap`, unaligned read/write | Rocks 내부에는 일부 C 메모리 연산 사용이 있으나, 전체를 일관되게 제공하는 공개 플랫폼 메모리 유틸리티는 확인하지 못하였다. 겹침 허용 여부와 alignment·타입 수명 규약을 정한다. | 길이 0, 동일 주소, 서로 겹치는 범위, 정렬되지 않은 주소, 표현 범위 경계, 실제 기대 바이트, sanitizer 및 플랫폼별 테스트 |
| P0 | 메모리 진단·페이지: 같은 헤더의 `GetStats`, `GetStatsRaw`, `GetConstants`, `PageProtect`, 가상 메모리 블록 | Rocks의 단일 영역 reserve/commit/decommit/release는 부분 구현이다. 읽기·쓰기 권한을 독립적으로 지정하는 보호 API와 물리·가용·프로세스·피크 메모리 및 압력 상태를 일관되게 조회하는 플랫폼 API는 후속이다. | 실제 페이지 접근 허용·차단을 양성·자식 프로세스 음성 대조로 확인한다. 범위·정렬·overflow, 실패 후 소유권과 재시도, OS 실패 주입, Windows/macOS 실기기 검증을 포함한다. |
| P0 | 동기 파일 I/O: `GenericPlatform/GenericPlatformFile.h`의 `IFileHandle`, `IPlatformFile`, `IPhysicalPlatformFile`, `HAL/PlatformFileManager.h` | `FileSystem`·`FileHandle`·소유형 `File`과 Windows/macOS 물리 backend의 열기·닫기·부분/정확 전송·seek·크기·존재·삭제가 초기 구현되었다. wrapper 체인 관리자·`ReadAt`·flush·truncate·상세 stat·디렉터리 순회는 후속이다. | 빈 파일·대형 파일·부분 I/O·EOF·경합·접근 거부·Unicode·잘못된 경로, 실패 후 리소스 회수와 wrapper의 호출 순서를 Windows/macOS에서 검증한다. 기존 Windows 심볼릭 링크 실행은 SKIP되었으나 개발자 모드 활성화 후 별도 재검증에서 통과했다. macOS 실기기·실제 OS close 실패 주입은 미검증이다. |
| P0 | 스레드 수명: `HAL/Thread.h`, `HAL/RunnableThread.h`, `GenericPlatform/GenericPlatformProcess.h` | Rocks는 raw entry/context 기반 `Thread::Start`, 완료 대기와 `Join`을 제공한다. 이름·우선순위·affinity·stack size의 지정·조회와 독립 소유 callable의 간편 사용은 후속이다. UE `FThread`의 `Detach`나 `FRunnableThread`의 suspend·kill과 동등한 기능을 현재 제공한다고 주장하지 않는다. | 생성 실패 시 자원 회수, context 소유·종료·자기 join·경합, 이름·우선순위·affinity 실패와 미지원, 수천 회 시작/회수 및 플랫폼 종료 동작을 검증한다. 분리 실행·강제 종료는 먼저 안전한 수명과 취소 계약을 확정한다. |
| P1 | 이벤트와 잠금: `HAL/Event.h`, `HAL/PThreadEvent.h`, `HAL/UESemaphore.h`, `HAL/PThreadsSharedMutex.h` | Rocks는 mutex·condition variable·manual-reset event와 프로세스 내부 semaphore를 제공한다. auto-reset event, event pool/소유형 편의 API, RW/shared mutex와 named interprocess semaphore는 후속이다. | 신호의 보존·소비 횟수, 다수 대기자·시간 제한·spurious wake·중복 reset, 소멸 전 대기 종료, 읽기/쓰기 경합과 굶주림 정책, 프로세스 간 생성·열기·권한·종료를 검증한다. |
| P1 | TLS: `GenericPlatform/GenericPlatformTLS.h`의 slot 할당·조회·설정·해제 | Rocks 내부에는 `thread_local`을 이용하는 구현이 있으나 UE에 대응하는 공개 동적 TLS slot API는 확인하지 못하였다. | slot 고갈·할당 실패·해제 후 재사용·스레드 간 격리·스레드 종료 정리·모듈 수명과 동시 경쟁을 검증한다. |
| P1 | 공유 메모리: `GenericPlatform/GenericPlatformMemory.h`의 named shared-memory map/unmap | Rocks에 named shared-memory 공개 API는 확인하지 못하였다. 범위와 읽기/쓰기 권한, 소유권과 플랫폼별 이름 규칙을 설계한다. | 별도 프로세스 간 읽기·쓰기, 중복 생성·열기, 접근 거부, 부분 실패·중도 종료·해제, 이름 충돌과 크기 경계를 검증한다. |
| P1 | 프로세스와 pipe: `GenericPlatform/GenericPlatformProcess.h`의 생성·대기·종료·반환값·pipe | **부분 구현(2026-11-13)**: `Platform/Process.h`의 생성·대기·종료 코드·자손까지 강제 종료·시간 제한 pipe·동시 수집 `RunProcess`·PID 생존 확인(위 "자식 프로세스와 pipe" 절). 환경 변수 지정·detach·(PID, 시작 시각) 정체·권한 상승 여부(Rockpile R0)·다른 스레드 취소는 후속이며 Android는 Unsupported다. | 실행 실패, stdin/stdout/stderr, 제한된 출력 수집, timeout·취소, 자식 및 자손 회수, 중복 종료·정리 실패와 플랫폼별 동작을 검증한다. Windows Debug·Asan과 Android 에뮬레이터 21건 통과, macOS는 CI만(로컬 미빌드). |
| P1 | 동적 모듈: `GenericPlatform/GenericPlatformProcess.h`의 라이브러리 load/export/free | Rocks `ModuleLease`는 이미 확보된 모듈 수명 관리에 사용되며 OS 라이브러리 로더 자체는 아니다. 로드·심볼 조회·해제와 callback 소유권을 별도로 구현한다. | 누락·잘못된 심볼, ABI, 참조 중 unload 거부, 동시 조회와 실패 rollback, Windows/macOS 실행을 검증한다. |
| P1 | 비동기·매핑 파일: `GenericPlatform/GenericPlatformFile.h`의 async read, mapped file 및 결과 오류 | 비동기 파일 핸들·매핑 파일 backend는 아직 없다. 동기 파일 계약과 task/lifetime 기반 이후에 도입한다. | 취소와 완료의 단일 종결, 사용 중 해제·늦은 completion, EOF·잘못된 범위·매핑 해제, fault injection과 실제 OS 실행을 검증한다. |
| P2 | 파일 journal 및 플랫폼 부가 기능: `GenericPlatform/GenericPlatformFile.h`의 journal 조회·반복, 파일 특성 | journal, 실제 디스크 이름·링크 및 플랫폼별 부가 메타데이터는 기능별 후속이다. unsupported는 성공이나 빈 결과로 위장하지 않는다. | 기능 가용성, 권한·미지원, 중단/재개와 변경 추적의 중복·누락, 플랫폼별 오류와 resource cleanup을 검증한다. |

우선순위는 의존성과 제품 위험에 따른 작업 순서다. 추가적인 UE 공개 기능 조사를 생략한다는 뜻은 아니다. 구현 후에는 기존 공통 계약 테스트와 함께 각 플랫폼의 실제 실행 증거를 연결하고, 미실행 환경은 미검증으로 유지한다.
