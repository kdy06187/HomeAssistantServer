#include "ApiAuth.hpp"
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <fstream>
#include <iostream>

namespace {
    const std::string kBearerPrefix = "Bearer ";
    // openssl rand -hex 32의 출력은 64자다. 짧은 토큰은 추측 위험이 있어 거부한다.
    constexpr size_t kMinTokenLength = 32;
}

std::optional<std::string> ApiAuth::loadToken(const std::string& tokenFilePath) {
    struct stat fileInfo{};
    if (stat(tokenFilePath.c_str(), &fileInfo) != 0) {
        std::cerr << "[ApiAuth] 토큰 파일을 찾을 수 없습니다: " << tokenFilePath << std::endl;
        return std::nullopt;
    }
    // SSH 개인키와 같은 정책: 소유자 외에 권한이 있으면 유출된 것으로 간주한다.
    if (fileInfo.st_mode & (S_IRWXG | S_IRWXO)) {
        std::cerr << "[ApiAuth] 토큰 파일 권한이 너무 넓습니다. chmod 600이 필요합니다." << std::endl;
        return std::nullopt;
    }
    std::ifstream tokenFile(tokenFilePath);
    std::string token;
    tokenFile >> token; // 파일 끝의 줄바꿈이 토큰에 포함되지 않도록 공백 단위로 읽는다.
    if (token.size() < kMinTokenLength) {
        std::cerr << "[ApiAuth] 토큰이 너무 짧습니다. 최소 길이: " << kMinTokenLength << std::endl;
        return std::nullopt;
    }
    return token;
}

bool ApiAuth::isAuthorized(const std::string& authorizationHeader, const std::string& expectedToken) {
    if (authorizationHeader.rfind(kBearerPrefix, 0) != 0) return false;
    const std::string presentedToken = authorizationHeader.substr(kBearerPrefix.size());
    // CRYPTO_memcmp는 길이가 같아야 한다. 토큰 길이는 비밀이 아니므로 먼저 반환해도 안전하다.
    if (presentedToken.size() != expectedToken.size()) return false;
    return CRYPTO_memcmp(presentedToken.data(), expectedToken.data(), expectedToken.size()) == 0;
}
