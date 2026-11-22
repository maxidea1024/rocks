#pragma once

#include <Compression/CompressedBlocks.h>

// 블록 컨테이너(Compression/CompressedBlocks.h)의 병렬 압축·병렬 읽기다. WorkerScheduler(Task
// 모듈)를 쓰므로 Compression 모듈이 아니라 AsyncCompression 모듈에 둔다. Compression은 Task보다
// 아래 계층이다 (Task가 TraceCapture를, TraceCapture가 Compression을 쓴다). 컨테이너 형식과 결과
// byte는 순차 API와 같다.
namespace rocks {

class WorkerScheduler;

// 병렬 압축이다. 결과 byte는 순차와 같다.
// - 연속 블록 범위 job을 worker와 호출 스레드가 함께 가져간다. 작업 제출이 거부되거나 scheduler가
//   Shutdown(Cancel)로 body를 건너뛰어도 호출 스레드가 남은 job을 처리하므로 결과는 완성된다.
// - 반환 시점: 호출 스레드가 job을 다 처리하면 아직 시작하지 않은 helper 작업은 회수하고
//   (WorkerTaskHandle::TryRevoke), 실행 중인 helper가 가져간 job(16MiB 이하)을 마칠 때까지만
//   기다린다. 그래서 worker가 다른 일로 바쁘거나 호출 스레드가 쥔 lock을 기다려도 끝난다.
// - job 결과를 모은 뒤 output을 한 번만 늘려 인덱스와 결과를 순서대로 복사한다. 실패하면 남은 job을
//   건너뛰고 가장 앞 블록의 오류를 돌려준다. helper가 코덱 문맥을 만들지 못하면 그 helper만
//   빠진다(호출 전체를 실패시키지 않는다).
// - scratch는 여러 스레드가 동시에 할당하고 다른 스레드가 해제해도 되는 할당기여야 한다
//   (System·Binned 등, Arena·Stack 금지).
// - 메모리: 참여 스레드(helper 최대 worker 수 + 호출 스레드)마다 코덱 문맥 하나다. zstd 고수준·큰
//   블록은 문맥이 크므로(수준 19·16MiB 블록은 약 81MiB) worker 수로 조절한다.
// - 블록 4개 이하이거나 1MiB 미만인 입력은 순차로 처리한다. helper 수는 원본 512KiB당 하나 이하다.
// - 호출 스레드가 그 scheduler의 worker면 WrongThread다(자기 worker를 기다리면 교착한다).
ROCKS_ASYNCCOMPRESSION_API Result<usize>
TryCompressBlocksAppend(WorkerScheduler& scheduler, const CompressedBlocksSettings& settings,
                        Span<const uint8> source, Array<uint8>& output,
                        Allocator& scratch) noexcept;

// 큰 범위 병렬 해제다. 블록은 destination에 바로 해제하고 걸치는 끝 블록만 임시 버퍼를 쓴다.
// 작업 분배·제출 거부·반환 시점·WrongThread·scratch 조건은 위 병렬 TryCompressBlocksAppend와 같다
// (1MiB 미만이거나 블록 4개 이하인 범위는 호출 스레드가 혼자 읽는다). 실패하면 가장 앞 블록의
// 오류이고 destination 내용은 미정이다.
// reader는 열린 CompressedBlocksReader이고 나머지 인자는 reader.TryRead(rawOffset, destination,
// stored, storedBase, context, verify)와 같다(읽기 문맥 대신 scratch에서 helper마다 해제 문맥을
// 만든다).
Result<void> TryReadCompressedBlocks(WorkerScheduler& scheduler,
                                     const CompressedBlocksReader& reader, uint64 rawOffset,
                                     Span<uint8> destination, Span<const uint8> stored,
                                     uint64 storedBase, Allocator& scratch,
                                     bool verify = true) noexcept;

} // namespace rocks
