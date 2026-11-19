#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "Result.hpp"

// Matter 수동 설정 코드(11·21자리 숫자)에서 PIN(setUpPINCode)을 꺼내는 함수의 모양.
// 실제 구현은 Matter SDK가 필요하므로 hac_drivers(MatterSetupCode.cpp)에 두고, 테스트는 가짜를 넘긴다
using SetupPinParser = std::function<Result<uint32_t>(const std::string& manualCode)>;

// Matter SDK의 ManualSetupPayloadParser를 쓰는 실제 구현(hac_drivers)
Result<uint32_t> parseManualSetupPin(const std::string& manualCode);
