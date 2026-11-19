#include "MatterSetupCode.hpp"
#include <iostream>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPError.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/SetupPayload.h>

// 이 파일만 Matter SDK에 의존한다. 그래서 MatterController는 SDK 없이 빌드·테스트된다
Result<uint32_t> parseManualSetupPin(const std::string& manualCode) {
    chip::SetupPayload payload;
    CHIP_ERROR err = chip::ManualSetupPayloadParser(manualCode).populatePayload(payload);
    if (err != CHIP_NO_ERROR) {
        std::cerr << "[MatterController] ❌ 핀 코드 파싱 실패. Error: " << err.Format() << std::endl;
        return Result<uint32_t>::failure(ErrorCode::CommissionFailed, "핀 코드 파싱 실패");
    }
    return Result<uint32_t>::success(payload.setUpPINCode);
}
