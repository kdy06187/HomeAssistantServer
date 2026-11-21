#include <gtest/gtest.h>
#include "HTTPServer.hpp"

namespace {

TEST(ParseHttpBindAddress, UnsetOrEmptyUsesAllInterfaces) {
    EXPECT_EQ(*parseHttpBindAddress(nullptr).value, "0.0.0.0");
    EXPECT_EQ(*parseHttpBindAddress("").value, "0.0.0.0");
}

TEST(ParseHttpBindAddress, AcceptsIpv4Addresses) {
    EXPECT_EQ(*parseHttpBindAddress("127.0.0.1").value, "127.0.0.1");
    EXPECT_EQ(*parseHttpBindAddress("192.168.0.5").value, "192.168.0.5");
    EXPECT_EQ(*parseHttpBindAddress("0.0.0.0").value, "0.0.0.0");
}

TEST(ParseHttpBindAddress, RejectsMalformedValues) {
    for (const char* bad : {"localhost", "::1", "256.0.0.1", "1.2.3", "1.2.3.4.5", " 1.2.3.4", "1.2.3.4\n",
                            "1.2.3.4:8000", "abc"}) {
        const auto result = parseHttpBindAddress(bad);
        EXPECT_FALSE(result.ok()) << bad;
    }
}

}  // namespace
