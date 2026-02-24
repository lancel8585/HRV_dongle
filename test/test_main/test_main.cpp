#include <Arduino.h>
#include <unity.h>

#include "time_utils.h" // Include the new header for nbiot_time_chang

// Test cases for nbiot_time_chang
void test_valid_time_string(void) {
    String input = "23/07/15,10:30:45+480";
    String expected = "2023-07-15T02:30:45.0";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), nbiot_time_chang(input).c_str());
}

void test_empty_time_string(void) {
    String input = "";
    String expected = "0000-00-00T00:00:00.0";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), nbiot_time_chang(input).c_str());
}

void test_time_string_no_offset(void) {
    String input = "24/01/01,12:00:00+0";
    String expected = "2024-01-01T12:00:00.0";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), nbiot_time_chang(input).c_str());
}

void test_time_string_negative_offset(void) {
    String input = "23/03/10,10:30:00-120";
    String expected = "2023-03-10T12:30:00.0";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), nbiot_time_chang(input).c_str());
}

// Retain these stubs. Even though main.cpp is no longer included,
// the PlatformIO test runner might still expect them for a test file named test_main.cpp,
// or if other test files link against it. It's safer to keep them.
// Alternatively, if this test file is simple and doesn't interact with a larger test suite structure
// that might require setup/loop, these could potentially be removed if test execution is successful without them.
// For now, keep them to be safe.
void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_valid_time_string);
    RUN_TEST(test_empty_time_string);
    RUN_TEST(test_time_string_no_offset);
    RUN_TEST(test_time_string_negative_offset);
    UNITY_END();
}

void loop() {
    // Keep empty
}
