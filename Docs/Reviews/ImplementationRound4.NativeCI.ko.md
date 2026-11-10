# 구현 검증 4차 — Windows/macOS native 실행

2026-11-10. 최종 통합 담당자가 GitHub Actions 실행과 내려받은 artifact를 직접 확인했다. 이전 서브에이전트의 독립 소스 검토와 구분되는 실제 플랫폼 실행 기록이다.

## 결과

검증 커밋은 `ea0537cd6b9ac0947300eabc92eb889e05b83bde`이며 [실행 42766665330](https://github.com/maxidea1024/rocks/actions/runs/42766665330)의 두 native job이 모두 success다.

| 구성 | CPU·compiler | 단위 테스트 | CTest 묶음 |
|---|---|---:|---:|
| Windows Debug | x64 · Clang 20.1.8 | 65/65 PASS | 5/5 PASS |
| macOS Debug | ARM64 · Apple Clang 17.0.0 | 65/65 PASS | 5/5 PASS |
| macOS ASan | ARM64 · Apple Clang 17.0.0 | 65/65 PASS | 6/6 PASS |

각 JSON의 failures는 0이다. String 13개를 포함한 전체 Foundation 집합을 실행했다. ASan은 의도적인 해제 후 접근 자식 프로세스가 sanitizer 진단과 함께 실패하는 음성 대조도 포함한다. 각 build-evidence에서 Git HEAD 일치, dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true를 확인했다. 동일한 소스 80개 digest는 `11c67a2cf4e1724942fd0895f2f9a99be73c9d5cef2e5cb576bbb13498e1ecbb`다.

artifact 원문은 로컬 `out/ci-42766665330/macos`, `out/ci-42766665330/windows`에도 보관했다. out은 생성 결과이므로 Git 소스에는 포함하지 않는다.

## 실행 중 수정한 사항

최초 실행 `42766520048`에서는 Windows job은 성공했으나 macOS 14 job은 실행 환경 미지원으로 시작 전에 취소됐다. [GitHub의 macOS 14 지원 종료 안내](https://github.blog/changelog/2026-10-01-github-actions-macos-14-runner-image-retirement/)와 현재 지원 label을 확인하고 `macos-15`로 수정했다. 런타임·테스트 코드는 바꾸지 않았다. 취소된 첫 실행을 Mac 코드 실패나 통과로 합산하지 않는다.

## 검증 범위

로컬 Windows Clang 21.1.6에서의 Core 38개, 전체 Debug/Release/ASan 각 65개와 Release 6,500회 반복도 앞선 기록대로 통과했다. 그 실행은 커밋 전 dirty 작업 트리 및 runner 변경 전 digest이므로 위 clean CI 증거와 구분한다.

이는 CPU 전용 Foundation subset에 대한 Windows x64/macOS ARM64 실행이다. Intel Mac, 렌더러·에디터·입력/IME, 전체 Task Graph, 점진·병렬 GC, Text 지역화, 전체 Foundation Gate의 완료를 뜻하지 않는다. 제품용 성능·임의 raw pointer 오용·모든 외부 라이브러리 수명까지 포괄하는 증명도 아니다.

## 마지막 교차 검토와 후속 회귀

빌드 담당 서브에이전트가 위 artifact와 문서를 독립 대조하여 수치·컴파일러·소스 해시·HEAD·상태의 일치를 확인했다. 다만 FoundationStatus에 보고서 쓰기 실패를 실행했다고 기술한 문구가 실제 fixture보다 넓다는 지적이 있었다.

`test_harness.py`에 기존 디렉터리를 `--json` 출력 파일로 지정하는 자식 프로세스 시험을 추가했다. exit 2, `Cannot open JSON report` 진단, 테스트 `[PASS]` 미출력을 함께 확인한다. 서브에이전트가 실패 분기와 fixture의 일치를 다시 검토했다. 출력 도중 디스크 부족·close 실패 시험은 여전히 미실행으로 구분했다.

Windows 로컬에서 새 하네스를 실행한 뒤 커밋 `556e5a28c9127d4929ae782350ae8a9dce112ec2`의 [최종 CI 실행 42767199402](https://github.com/maxidea1024/rocks/actions/runs/42767199402)에서도 Windows Debug 65개/CTest 5개, macOS Debug 65개/5개, macOS ASan 65개/6개가 모두 통과했다. 각 실패 수는 0이다. 원문 artifact는 `out/ci-42767199402`에 내려받아 확인했다.

세 구성의 최종 소스 digest는 `8843d7f11f521d648b2b442a36c980a37a1117f52f3564f2a835f3608bb29a83`이며 dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true다. 런타임 C++ 변경 없이 실패 회귀와 문서만 보강한 후속 검증이다.
