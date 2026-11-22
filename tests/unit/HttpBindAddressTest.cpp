#include <gtest/gtest.h>
#include "RequestValidation.hpp"

namespace {

TEST(ParseBindAddress, UnsetOrEmptyUsesAllInterfaces) {
    EXPECT_EQ(*RequestValidation::parseBindAddress(nullptr).value, "0.0.0.0");
    EXPECT_EQ(*RequestValidation::parseBindAddress("").value, "0.0.0.0");
}

TEST(ParseBindAddress, AcceptsIpv4Addresses) {
    EXPECT_EQ(*RequestValidation::parseBindAddress("127.0.0.1").value, "127.0.0.1");
    EXPECT_EQ(*RequestValidation::parseBindAddress("192.168.0.5").value, "192.168.0.5");
    EXPECT_EQ(*RequestValidation::parseBindAddress("0.0.0.0").value, "0.0.0.0");
}

TEST(ParseBindAddress, RejectsMalformedValues) {
    for (const char* bad : {"localhost", "::1", "256.0.0.1", "1.2.3", "1.2.3.4.5", " 1.2.3.4", "1.2.3.4\n",
                            "1.2.3.4:8000", "abc"}) {
        const auto result = RequestValidation::parseBindAddress(bad);
        ASSERT_FALSE(result.ok()) << bad;
        EXPECT_EQ(result.error.code, ErrorCode::InvalidRequest) << bad;
    }
}

}  // namespace
