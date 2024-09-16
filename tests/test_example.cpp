/*
 * Created: 2024/9/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <gtest/gtest.h>

// A simple example test
TEST(ExampleTest, Test1) {
    EXPECT_EQ(1, 1);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}