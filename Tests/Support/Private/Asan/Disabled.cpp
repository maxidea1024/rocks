namespace rocks::test {

int RunAsanNegativeControl() noexcept {
    // 비계측 구성에서 의도적인 UAF를 실행하지 않는다.
    return 2;
}

} // namespace rocks::test
