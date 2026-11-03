---
description: Run unit, E2E, monkey and user tests thoroughly after an implementation or change
---

# Thorough test run

After any implementation or change, follow this command to run the four test categories defined in `docs/testing.md` (unit tests, E2E tests, monkey tests, user tests) according to the impact of the change.

For details, procedures and the full scenario list, see [docs/testing.md](../../docs/testing.md).

## Steps

1. **Identify the impact**
   - Check which files and modules changed and decide which test categories in `docs/testing.md` are affected.

2. **Run the unit tests (required)**
   ```powershell
   cmake --build build --target publisher_tests -j 4
   .\build\tests\publisher_tests.exe
   ```
   - Confirm that `All tests passed.` is printed.
   - If you added or changed logic, add or update the matching test cases in `tests/unit/`, then run again and confirm they pass.

3. **Run the E2E tests (for changes to the state machine, RTSP publishing or the encoder)**
   ```powershell
   cd e2e-test
   pwsh ./run-tests.ps1
   pwsh ./monkey-tests.ps1
   ```
   - Confirm that everything is `[PASS]`.

4. **Add monkey tests (when adding new abnormal-case scenarios or config options)**
   - Add the matching test case to `e2e-test/monkey-tests.ps1`, run it and confirm `[PASS]`.
   - For items that need a GPU or a real RTSP server and are hard to automate, add them to "Additional scenarios to check manually" in `docs/testing.md`.

5. **User test (for changes that affect production-like behaviour)**
   - Do a final check with a real Spout source and an RTSP server (for example `rtsp://192.168.0.100:8554/spout-test`; use your own server address).
   - Check the contents of `state/health.json`, `state/metrics.json` and `logs/*.jsonl`.

6. **Report the results**
   - Report the test categories run, counts and results (PASS/FAIL) to the user.
   - If any `[FAIL]` occurs, investigate and fix the cause, then run all the tests again.

## Notes

- If you skip tests or run only some of them, state the reason explicitly.
- When you add new logic, branches or config options, always add the matching test cases (existing tests passing is not enough).
- Do not put sensitive information (real IP addresses, credentials, etc.) in test code or logs.
