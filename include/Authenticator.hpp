#pragma once
#include <string>
#include <utility>
#include "ApiAuth.hpp"

// 요청 인증 판정 전략. HTTPServer는 이 인터페이스만 알아서, 테스트에서 가짜를 넣거나 방식을 바꿀 수 있다.
class Authenticator {
public:
    virtual ~Authenticator() = default;
    virtual bool isAuthorized(const std::string& authorizationHeader) const = 0;
};

// 기존 ApiAuth(Bearer 토큰 비교)를 감싼 구현
class BearerTokenAuthenticator final : public Authenticator {
public:
    explicit BearerTokenAuthenticator(std::string expectedToken) : mExpectedToken(std::move(expectedToken)) {}
    bool isAuthorized(const std::string& authorizationHeader) const override {
        return ApiAuth::isAuthorized(authorizationHeader, mExpectedToken);
    }

private:
    std::string mExpectedToken;
};
