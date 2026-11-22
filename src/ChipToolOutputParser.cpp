#include "ChipToolOutputParser.hpp"
#include <cctype>
#include <charconv>
#include <sstream>
#include <system_error>
#include <vector>

namespace ChipToolOutputParser {

namespace {
    bool containsAny(const std::string& text, const std::vector<std::string>& keywords) {
        for (const auto& keyword : keywords) {
            if (text.find(keyword) != std::string::npos) return true;
        }
        return false;
    }

    // key 뒤의 값을 줄 단위로 찾는다. 키가 있는 첫 줄에서 멈춘다.
    std::optional<std::string> extractTrimmedValueAfterKey(const std::string& output, const std::string& key) {
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            size_t position = line.find(key);
            if (position == std::string::npos) continue;
            std::string value = line.substr(position + key.size());
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \n\r\t") + 1);
            return value;
        }
        return std::nullopt;
    }

    // 앞쪽 공백·따옴표는 건너뛰고, 공백·따옴표·제어문자(ESC 등)에서 끊는다.
    std::string extractCleanToken(const std::string& raw) {
        std::string clean;
        size_t start = raw.find_first_not_of(" \t\"\'");
        if (start == std::string::npos) return clean;
        for (size_t i = start; i < raw.length(); ++i) {
            char c = raw[i];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '\"' || c == '\'' || c == '\033' || c < 32) break;
            clean += c;
        }
        return clean;
    }

    bool isAllDigits(const std::string& text) {
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }
}

Result<int64_t> parseInteger(const std::string& text) {
    size_t index = 0;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    // from_chars는 '+'를 받지 않으므로 직접 건너뛴다. 이때 '-'가 이어지면(+-5) 실패여야 한다.
    bool hasPlusSign = index < text.size() && text[index] == '+';
    if (hasPlusSign) ++index;

    size_t digitsStart = index;
    if (!hasPlusSign && digitsStart < text.size() && text[digitsStart] == '-') ++digitsStart;
    if (digitsStart >= text.size() || !std::isdigit(static_cast<unsigned char>(text[digitsStart]))) {
        return Result<int64_t>::failure(ErrorCode::Internal, "정수로 해석할 수 없는 출력");
    }

    int64_t parsed = 0;
    const char* begin = text.data() + index;
    const char* end = text.data() + text.size();
    auto converted = std::from_chars(begin, end, parsed);
    if (converted.ec != std::errc()) {
        return Result<int64_t>::failure(ErrorCode::Internal, "정수 범위 초과");
    }
    return Result<int64_t>::success(parsed);
}

bool hasCommandSuccessMarker(const std::string& output) {
    return containsAny(output, {
        "CHIP_NO_ERROR",
        "status 0x00 (SUCCESS)",
        "status = 0x00",
        "Success status report"
    });
}

std::optional<std::string> findCommandErrorKeyword(const std::string& output) {
    const std::vector<std::string> errorKeywords = {
        "Timeout",
        "Error resolving node",
        "AccessDenied",
        "Incorrect state",
        "CHIP_ERROR_"
    };
    for (const auto& keyword : errorKeywords) {
        if (output.find(keyword) != std::string::npos) return keyword;
    }
    return std::nullopt;
}

std::optional<std::string> parseOnOffState(const std::string& output) {
    if (containsAny(output, {"Data = true", "Data: 1"})) return std::string("ON");
    if (containsAny(output, {"Data = false", "Data: 0"})) return std::string("OFF");
    return std::nullopt;
}

std::optional<std::string> extractActivePowerText(const std::string& output) {
    return extractTrimmedValueAfterKey(output, "ActivePower:");
}

std::optional<std::string> extractEnergyText(const std::string& output) {
    return extractTrimmedValueAfterKey(output, "Energy:");
}

std::optional<std::string> extractSerialNumber(const std::string& output) {
    const std::string serialKey = "SerialNumber:";
    size_t position = output.find(serialKey);
    if (position != std::string::npos) {
        std::string serial = extractCleanToken(output.substr(position + serialKey.size()));
        if (!serial.empty()) return serial;
    }

    const std::string dataKey = "Data =";
    position = output.find(dataKey);
    if (position != std::string::npos) {
        std::string serial = extractCleanToken(output.substr(position + dataKey.size()));
        if (!serial.empty()) return serial;
    }
    return std::nullopt;
}

bool isCommissioningSuccessful(const std::string& output) {
    return containsAny(output, {"Device commissioning completed with success", "CHIP_NO_ERROR"});
}

Result<std::pair<std::string, uint64_t>> parseRegisteredDeviceLine(const std::string& line) {
    using LineResult = Result<std::pair<std::string, uint64_t>>;

    size_t commaPosition = line.find(',');
    if (commaPosition == std::string::npos) {
        return LineResult::failure(ErrorCode::InvalidRequest, "콤마 없음");
    }
    std::string serial = line.substr(0, commaPosition);
    if (serial.empty()) {
        return LineResult::failure(ErrorCode::InvalidRequest, "시리얼 비어 있음");
    }

    std::string nodeText = line.substr(commaPosition + 1);
    nodeText.erase(0, nodeText.find_first_not_of(" \t"));
    nodeText.erase(nodeText.find_last_not_of(" \t\r\n") + 1);
    if (!isAllDigits(nodeText)) {
        return LineResult::failure(ErrorCode::InvalidRequest, "노드 ID가 숫자가 아님");
    }

    uint64_t nodeId = 0;
    auto converted = std::from_chars(nodeText.data(), nodeText.data() + nodeText.size(), nodeId);
    if (converted.ec != std::errc()) {
        return LineResult::failure(ErrorCode::InvalidRequest, "노드 ID 범위 초과");
    }
    return LineResult::success({serial, nodeId});
}

bool isUnreachableMarker(const std::string& text) {
    return text == "OFFLINE" || text == "PAIRING";
}

Result<EnergyReading> parseEnergyReading(const std::string& activePowerText, const std::string& cumulativeText) {
    if (isUnreachableMarker(activePowerText) || isUnreachableMarker(cumulativeText)) {
        return Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "기기 오프라인 또는 페어링 중");
    }
    // 누적값은 통계의 기준이 되므로 "값 없음"을 0으로 바꾸지 않는다
    if (cumulativeText == "UNKNOWN" || cumulativeText.empty()) {
        return Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 없음");
    }

    EnergyReading reading{0, 0};
    if (activePowerText != "UNKNOWN" && !activePowerText.empty()) {
        const auto parsedPower = parseInteger(activePowerText);
        if (!parsedPower.ok()) return Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "실시간 전력 값 해석 실패");
        reading.activePower_mW = *parsedPower.value;
    }
    const auto parsedEnergy = parseInteger(cumulativeText);
    if (!parsedEnergy.ok()) return Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "누적 전력 값 해석 실패");
    if (*parsedEnergy.value < 0) return Result<EnergyReading>::failure(ErrorCode::DeviceUnreachable, "음수 누적 전력 값");
    reading.cumulative_mWh = *parsedEnergy.value;
    return Result<EnergyReading>::success(reading);
}

} // namespace ChipToolOutputParser
