#include "Result.hpp"
#include <gtest/gtest.h>

TEST(ResultTest, SuccessHoldsValue) {
    auto result = Result<int>::success(7);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(*result.value, 7);
    EXPECT_EQ(result.error.code, ErrorCode::None);
}

TEST(ResultTest, FailureHasCodeAndNoValue) {
    auto result = Result<int>::failure(ErrorCode::NotFound, "없음");
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.value.has_value());
    EXPECT_EQ(result.error.code, ErrorCode::NotFound);
    EXPECT_EQ(result.error.message, "없음");
}

TEST(ResultTest, StatusOkOnlyWhenCodeIsNone) {
    EXPECT_TRUE(Status::success().ok());
    auto failed = Status::failure(ErrorCode::Internal, "x");
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(failed.error.code, ErrorCode::Internal);
}
