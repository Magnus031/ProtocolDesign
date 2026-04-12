#include <gtest/gtest.h>

// Simple test to verify GoogleTest is working
TEST(GoogleTestVersion, CompilesAndRuns) {
    EXPECT_TRUE(true);  // Basic assertion
}

TEST(GoogleTestVersion, SimpleMath) {
    EXPECT_EQ(1 + 1, 2);
}

// Optional: Print GoogleTest version if available
TEST(GoogleTestVersion, PrintVersion) {
    // These macros might not be available in all versions
    // Just check that we can compile and run
    SUCCEED() << "GoogleTest 1.17.0 is working with C++17";
}