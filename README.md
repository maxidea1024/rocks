# Rocks

C++23 기반 멀티플랫폼 3D 게임 엔진 프로젝트.

현재 **Core 기반 라이브러리와 Object 시스템의 첫 구현**을 진행하고 있다. 자체 할당기·컨테이너·String/Name/Text, worker 작업, 생성 리플렉션, 생성 트랜잭션, 객체 그래프 직렬화, 비이동 점진·병렬 GC를 CPU 전용 테스트 호스트에서 실행한다. 전체 Foundation 및 제품용 엔진의 완료와는 구분한다. 실제 범위·검증·후속 작업은 [구현 상태](Docs/Implementation/FoundationStatus.ko.md)를 따른다.

## 구현 원칙

- 안정성·정확성을 먼저 확보하고 확장성과 성능을 검증한다.
- 엔진 소유 Runtime과 공개 API는 STL을 사용하지 않는다. 문자열도 자체 `String`/`StringView`를 사용하며 이름 intern `Name`과 지역화 `Text`도 자체 구현하고 의존성을 분리한다.
- **사람이 알아보기 좋은 코드 스타일 유지.** 스페이스 4칸, 적절한 빈 줄, 명확한 이름과 책임을 사용하며 탭 문자와 불필요한 한 줄 압축을 금지한다.
- 공개 API의 소유권·수명·스레드·실패 계약, 비자명한 구현 선택의 이유를 한국어 주석으로 설명한다. 주석은 구현과 함께 검토하고 갱신한다.
- 자체 `rocks` 네임스페이스를 사용하며 U/F/A 등 타입 분류용 접두사는 사용하지 않는다.
- Blueprint는 구현하지 않는다. 게임 로직은 C++와 자체 언어를 기준으로 설계한다. 재질 및 UI 등의 전문 편집기는 별도로 구현한다.
- **컴파일러 경고 무허용.** 경고는 오류로 처리하며 로컬·CI와 모든 구성에 같은 [필수 정책](Docs/Design/CompilerDiagnostics.ko.md)을 적용한다.
- 각 구현에 유닛·계약·장애 주입·수명·동시성 하네스와 재현 가능한 검증 증거를 연결한다.

## 빌드·검증

Windows/macOS native C++ 도구, CMake, Ninja, Python 환경에서 다음 진입점을 사용한다. 자세한 요구사항과 옵션은 [빌드 안내](Build/README.md)를 참고한다.

```text
python Build/build.py
python Build/format.py --check
```

기본 빌드는 해당 호스트의 Debug preset이다. Release/ASan/Core-only 옵션과 실제 플랫폼별 성공 여부는 구현 상태에 기록한다. CI 설정이 존재한다는 사실만으로 모든 플랫폼 검증 완료를 의미하지 않는다.

## 설계와 구현 문서

- [현재 구현 상태·수명 계약·테스트](Docs/Implementation/FoundationStatus.ko.md)
- [String·StringView·Name·Text 역할](Docs/Implementation/StringAndText.ko.md)
- [점진·병렬 GC 계약](Docs/Implementation/IncrementalGarbageCollection.ko.md)
- [Clang 생성 Reflection](Docs/Implementation/ReflectionMetadata.ko.md)
- [Archive·Record·객체 그래프 직렬화](Docs/Implementation/Serialization.ko.md)
- [WorkerScheduler 수명·종료 계약](Docs/Implementation/WorkerScheduler.ko.md)
- [전체 구현 계획](Docs/Design/EngineImplementationPlan.ko.md)
- [Core·Object·Input 상세 구현 계획](Docs/Design/CoreObjectInputDetailedPlan.ko.md)
- [작업 목록](Docs/Design/CoreObjectInputTasks.ko.csv)
- [예정 파일 구조](Docs/Design/CoreObjectInputFiles.ko.csv)
- [다회차 설계 검증 결과](Docs/Reviews/DesignReview.ko.md)
- [구현 교차 검토 기록](Docs/Reviews/)
- [코드 스타일·주석 기준](Docs/CodingStyle.md)
- [설계 PDF 배포본](Docs/Releases/)

작업 목록과 파일 구조는 구현 계획이다. 해당 파일이나 API가 모두 구현되었다는 의미가 아니다. 설계 검토 통과와 실제 컴파일·실행·성능 시험 통과를 구분한다.
