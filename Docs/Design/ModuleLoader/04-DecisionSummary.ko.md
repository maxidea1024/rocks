## 4. 결정 요약

1. 모든 C++ 모듈은 수명 객체(`ModuleImplementation`)와 진입점 매크로를 가진다. `Startup`은 `Result<void>`를 돌려준다.
2. 진입점은 전역 생성자 없이 상수 기술 + 함수다. Monolithic은 빌드 생성 모듈 표, Modular는 공유 라이브러리마다 export 진입점 하나다.
3. 로더는 Core 런타임 모듈 `ModuleLoader`이고 Launch가 소유한다. 전역 접근자는 없다.
4. 모듈은 로딩 단계에 따라 시작하고, 의존은 로더가 먼저 시작한다. 단계는 프로세스·호스트·엔진 세 묶음으로 나뉘고, 묶음마다 PreUnload 전부 → Shutdown 역순으로 내린다. 엔진 묶음은 Engine 수명에 묶인다.
5. Modular는 manifest·`ModuleBuildId`(UE처럼 재사용)·파일 크기로 바이너리를 찾고 맞는지 검사한다. 실행 파일은 부트 모듈만 import하고 본문은 호스트 모듈에 둔다. 실패는 원래 OS 오류를 담은 report다.
6. 모든 모듈의 설정 등록은 HostSettings가 설정을 확정하기 전에 한다.
7. 리플렉션 타입 등록은 레지스트리 주인이 타입 등록기로 하고, 로더가 실제 `ModuleLease`를 발급한다.
8. 실행 중 해제는 의존 모듈·타입 인스턴스가 남으면 아무것도 바꾸지 않고 거부하고, lease가 남으면 공유 라이브러리 해제를 미룬다.
