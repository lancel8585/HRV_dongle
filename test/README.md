# Unit Testing and Coverage

This directory contains unit tests for parts of the project.

## Testable Code

Due to the nature of embedded projects with direct hardware interactions, not all code is easily unit-testable.
The function `nbiot_time_chang` (responsible for parsing and converting time strings) has been isolated into `src/time_utils.h` and `src/time_utils.cpp` to make it testable. Tests for this function can be found in `test/test_main/test_main.cpp`.

The main application logic in `src/main.cpp` involves significant hardware interaction and is not covered by these unit tests.

## Running Tests

Tests are managed using PlatformIO and the Unity test framework. To run the tests, use the following command from the project root directory:

```bash
pio test -e 4d_systems_esp32s3_gen4_r8n16
```

The `platformio.ini` file has been configured with `build_flags = --coverage` to enable code coverage generation.

## Coverage Reports

After running the tests, a code coverage report should be generated.
- An `lcov.info` file can be found at `.pio/build/4d_systems_esp32s3_gen4_r8n16/lcov.info`.
- An HTML report, viewable in a web browser, should be available at `.pio/build/4d_systems_esp32s3_gen4_r8n16/coverage/html/index.html`. This report will show line-by-line coverage for `src/time_utils.cpp`.

**NOTE ON EXECUTION DURING DEVELOPMENT (2025-05-29):**
During the development of this test setup, execution of `pio test` commands was hindered by issues with the PlatformIO CLI environment itself (commands produced no output). The instructions above reflect how the process is intended to work in a correctly configured and functional PlatformIO environment. If you encounter issues, please ensure your PlatformIO installation is working correctly.
