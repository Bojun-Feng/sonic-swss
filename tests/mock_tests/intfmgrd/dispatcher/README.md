# Interface-manager dispatcher regression

Run `python3 tests/mock_tests/intfmgrd/dispatcher/run.py --out /path/to/new/build-dir` from a checkout. Set `CXX` to choose a C++14 compiler. `--baseline <pre-fix-git-revision>` additionally executes the same assertions against the original dispatcher: the four busy-event fairness cases must fail while the five controls pass. Candidate and UBSan runs must pass all nine cases. A baseline revision is optional so shallow CI checkouts do not require unrelated history.

The harness compiles the complete, unmodified production `intfmgrd.cpp` and extracts four complete production methods (Orch retry sweep, Consumer execute/drain, and the IntfMgr per-consumer queue handler). It records input and definition digests. The fixture supplies external dependencies and a finite selected-event schedule. Readiness is an injected dependency boundary, not a simulated Redis/FRR/SAI implementation. The unchanged production loop is stopped only after all scheduled events have been dispatched.

Assertions cover continuously selected unrelated PORT/LAG events, multiple interface tables, callback-before-retry ordering, a prerequisite that remains blocked, empty work, idle timeouts, same-table events, and select errors. The fixture also bounds retry attempts to catch an internal busy loop and verifies unrelated callbacks are still serviced.

This test does not certify event coalescing, route-ring ordering, warm restart, removal cleanup, RIF retirement or full daemon integration. Those require their own tests. The fixture is intentionally specific to the dispatcher regression, not a replacement test framework.
