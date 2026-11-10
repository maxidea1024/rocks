namespace Rocks.Tooling.Common;

// 한 프로세스의 실행과 지연 정리가 보유하는 용량이다. 정리가 완료되지 않은 경우도
// 차지한 상태로 남으므로 분리된 후손이 무한히 pipe를 보유해도 리소스 누적은 상한이 있다.
internal sealed class ProcessCleanupBudget(int maximum)
{
    private readonly SemaphoreSlim slots = maximum > 0 ? new(maximum, maximum) :
        throw new ArgumentOutOfRangeException(nameof(maximum));

    public Reservation? TryReserve() => slots.Wait(0) ? new Reservation(slots) : null;

    internal sealed class Reservation(SemaphoreSlim slots)
    {
        private int attached;

        public void ReleaseAfter(Task cleanup)
        {
            if (Interlocked.Exchange(ref attached, 1) != 0)
                throw new InvalidOperationException("Process cleanup reservation is already attached.");
            _ = cleanup.ContinueWith(completed =>
            {
                _ = completed.Exception; // 나중에 fault해도 관찰한다.
                slots.Release();
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
    }
}
