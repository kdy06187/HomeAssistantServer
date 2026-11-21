#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include "ApiAuth.hpp"

namespace {
const std::string kToken(64, 'a');

TEST(ApiAuthIsAuthorized, ExactBearerTokenIsAccepted) {
    EXPECT_TRUE(ApiAuth::isAuthorized("Bearer " + kToken, kToken));
}

TEST(ApiAuthIsAuthorized, MissingPrefixIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized(kToken, kToken));
}

TEST(ApiAuthIsAuthorized, LowercasePrefixIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("bearer " + kToken, kToken));
}

TEST(ApiAuthIsAuthorized, DifferentTokenSameLengthIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer " + std::string(64, 'b'), kToken));
}

TEST(ApiAuthIsAuthorized, DifferentLengthIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer " + kToken + "a", kToken));
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer " + kToken.substr(1), kToken));
}

TEST(ApiAuthIsAuthorized, TrailingOrExtraSpaceIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer " + kToken + " ", kToken));
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer  " + kToken, kToken));
}

TEST(ApiAuthIsAuthorized, EmptyHeaderIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("", kToken));
}

TEST(ApiAuthIsAuthorized, PrefixOnlyIsRejected) {
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer ", kToken));
}

TEST(ApiAuthIsAuthorized, EmptyExpectedTokenRejectsEverything) {
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer ", ""));
    EXPECT_FALSE(ApiAuth::isAuthorized("", ""));
    EXPECT_FALSE(ApiAuth::isAuthorized("Bearer anything", ""));
}

// 임시 파일을 만들고 소멸자에서 지운다. 실제 config/ 는 건드리지 않는다.
class TokenFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        char pathTemplate[] = "/tmp/hac_token_XXXXXX";
        int fd = mkstemp(pathTemplate);
        ASSERT_NE(fd, -1);
        close(fd);
        path_ = pathTemplate;
    }
    void TearDown() override { unlink(path_.c_str()); }

    void writeFile(const std::string& content, mode_t mode) {
        std::ofstream(path_, std::ios::trunc) << content;
        chmod(path_.c_str(), mode);
    }

    std::string path_;
};

TEST_F(TokenFileTest, OwnerOnlyFileReturnsToken) {
    writeFile(kToken + "\n", 0600);
    auto token = ApiAuth::loadToken(path_);
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, kToken); // 줄바꿈은 토큰에 포함되지 않는다
}

TEST_F(TokenFileTest, GroupOrOtherReadableFileIsRejected) {
    writeFile(kToken, 0644);
    EXPECT_FALSE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, TooShortTokenIsRejected) {
    writeFile(std::string(31, 'a'), 0600);
    EXPECT_FALSE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, MinimumLengthTokenIsAccepted) {
    writeFile(std::string(32, 'a'), 0600);
    EXPECT_TRUE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, EmptyFileIsRejected) {
    writeFile("", 0600);
    EXPECT_FALSE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, SurroundingWhitespaceIsStripped) {
    writeFile("  \n" + kToken + "  \n", 0600);
    auto token = ApiAuth::loadToken(path_);
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, kToken);
}

TEST_F(TokenFileTest, ReadOnlyOwnerFileIsAccepted) {
    writeFile(kToken, 0400);
    EXPECT_TRUE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, GroupWritableFileIsRejected) {
    writeFile(kToken, 0620);
    EXPECT_FALSE(ApiAuth::loadToken(path_).has_value());
}

TEST_F(TokenFileTest, OwnFileProducesNoOwnerWarning) {
    writeFile(kToken, 0600);
    testing::internal::CaptureStderr();
    EXPECT_TRUE(ApiAuth::loadToken(path_).has_value());
    EXPECT_EQ(testing::internal::GetCapturedStderr().find("소유자"), std::string::npos);
}

TEST_F(TokenFileTest, OtherOwnerWarnsButStillReturnsToken) {
    writeFile(kToken, 0600);
    testing::internal::CaptureStderr();
    const auto token = ApiAuth::loadToken(path_, geteuid() + 1);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(token.has_value());
    EXPECT_NE(log.find("소유자"), std::string::npos) << log;
    EXPECT_EQ(log.find(kToken), std::string::npos) << "경고에 토큰이 들어가면 안 됨";
}

TEST(ApiAuthLoadToken, MissingFileReturnsNullopt) {
    EXPECT_FALSE(ApiAuth::loadToken("/tmp/hac_no_such_token_file").has_value());
}
}  // namespace
