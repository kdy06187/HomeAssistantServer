#pragma once
#include "Result.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// chip-tool 출력 문자열을 해석하는 순수 함수 모음. 예외를 던지지 않는다.
namespace ChipToolOutputParser {

// std::stoll과 같은 앞부분 규칙: 앞 공백 무시, 부호 선택, 숫자 1개 이상, 뒤 문자는 무시.
Result<int64_t> parseInteger(const std::string& text);

// 성공 표지가 하나라도 있으면 true
bool hasCommandSuccessMarker(const std::string& output);

// 출력에서 처음 발견되는 오류 키워드. 없으면 nullopt
std::optional<std::string> findCommandErrorKeyword(const std::string& output);

// "Data = true"/"Data: 1" -> "ON", "Data = false"/"Data: 0" -> "OFF", 그 외 nullopt
std::optional<std::string> parseOnOffState(const std::string& output);

// "ActivePower:" 뒤의 값(첫 번째로 발견된 줄, 앞뒤 공백 제거). 없으면 nullopt
std::optional<std::string> extractActivePowerText(const std::string& output);

// "Energy:" 뒤의 값(첫 번째로 발견된 줄, 앞뒤 공백 제거). 없으면 nullopt
std::optional<std::string> extractEnergyText(const std::string& output);

// "SerialNumber:" 뒤, 없으면 "Data =" 뒤의 첫 토큰. 없으면 nullopt
std::optional<std::string> extractSerialNumber(const std::string& output);

bool isCommissioningSuccessful(const std::string& output);

// "시리얼,노드ID" 한 줄. 줄 끝의 \r·공백은 허용하고, 노드ID는 숫자만 허용한다.
Result<std::pair<std::string, uint64_t>> parseRegisteredDeviceLine(const std::string& line);

} // namespace ChipToolOutputParser
