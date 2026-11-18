#include "ChipToolOutputParser.hpp"
#include <gtest/gtest.h>

using namespace ChipToolOutputParser;

TEST(ParseIntegerTest, ValidInputs) {
    EXPECT_EQ(*parseInteger("  15230 ").value, 15230);
    EXPECT_EQ(*parseInteger("-5").value, -5);
    EXPECT_EQ(*parseInteger("+7").value, 7);
    EXPECT_EQ(*parseInteger("12abc").value, 12);
}

TEST(ParseIntegerTest, InvalidInputsFailWithInternal) {
    for (const char* text : {"OFFLINE", "PAIRING", "ERROR", "", "   ", "+-5", "-", "9223372036854775808"}) {
        auto result = parseInteger(text);
        EXPECT_FALSE(result.ok()) << text;
        EXPECT_EQ(result.error.code, ErrorCode::Internal) << text;
    }
}

TEST(CommandJudgeTest, SuccessMarkers) {
    EXPECT_TRUE(hasCommandSuccessMarker("... CHIP_NO_ERROR ..."));
    EXPECT_TRUE(hasCommandSuccessMarker("status 0x00 (SUCCESS)"));
    EXPECT_TRUE(hasCommandSuccessMarker("IM:InvokeCommandResponse status = 0x00"));
    EXPECT_TRUE(hasCommandSuccessMarker("Success status report received"));
    EXPECT_FALSE(hasCommandSuccessMarker(""));
    EXPECT_FALSE(hasCommandSuccessMarker("Run command failure"));
}

TEST(CommandJudgeTest, ErrorKeywordFirstInListOrder) {
    EXPECT_EQ(findCommandErrorKeyword("CHIP_NO_ERROR ok"), std::nullopt);
    EXPECT_EQ(*findCommandErrorKeyword("x Timeout y"), "Timeout");
    EXPECT_EQ(*findCommandErrorKeyword("CHIP_ERROR_FOO and Timeout"), "Timeout");
    EXPECT_EQ(*findCommandErrorKeyword("Error resolving node 5"), "Error resolving node");
    EXPECT_EQ(*findCommandErrorKeyword("AccessDenied"), "AccessDenied");
    EXPECT_EQ(*findCommandErrorKeyword("Incorrect state"), "Incorrect state");
    EXPECT_EQ(*findCommandErrorKeyword("CHIP_ERROR_INTERNAL"), "CHIP_ERROR_");
}

TEST(OnOffTest, Variants) {
    EXPECT_EQ(*parseOnOffState("OnOff: Data = true (bool)"), "ON");
    EXPECT_EQ(*parseOnOffState("Data: 1"), "ON");
    EXPECT_EQ(*parseOnOffState("Data = false"), "OFF");
    EXPECT_EQ(*parseOnOffState("Data: 0"), "OFF");
    EXPECT_EQ(parseOnOffState("Data = maybe"), std::nullopt);
}

TEST(ValueExtractTest, ActivePowerAndEnergy) {
    const std::string output = "line1\n[1] ActivePower:   1500 \r\nActivePower: 9\n";
    EXPECT_EQ(*extractActivePowerText(output), "1500");
    EXPECT_EQ(*extractEnergyText("a\n Energy: 42\t\nb"), "42");
    EXPECT_EQ(extractActivePowerText("nothing"), std::nullopt);
    EXPECT_EQ(*extractEnergyText("Energy:   \n"), ""); // 키만 있고 값이 비면 빈 문자열
}

TEST(SerialTest, PrimaryAndFallback) {
    EXPECT_EQ(*extractSerialNumber("SerialNumber: \"ABC123\"\n"), "ABC123");
    EXPECT_EQ(*extractSerialNumber("Data = 'XYZ-9' (string)"), "XYZ-9");
    EXPECT_EQ(*extractSerialNumber("SerialNumber: \nData = FALLBACK\n"), "FALLBACK");
    EXPECT_EQ(extractSerialNumber("no serial"), std::nullopt);
}

TEST(CommissionTest, SuccessMarkers) {
    EXPECT_TRUE(isCommissioningSuccessful("Device commissioning completed with success"));
    EXPECT_TRUE(isCommissioningSuccessful("CHIP_NO_ERROR"));
    EXPECT_FALSE(isCommissioningSuccessful("Commissioning failed"));
}

TEST(RegisteredLineTest, Valid) {
    auto plain = parseRegisteredDeviceLine("SN1,123");
    ASSERT_TRUE(plain.ok());
    EXPECT_EQ(plain.value->first, "SN1");
    EXPECT_EQ(plain.value->second, 123u);
    EXPECT_TRUE(parseRegisteredDeviceLine("SN1,123\r").ok());
    EXPECT_EQ(parseRegisteredDeviceLine("SN1,18446744073709551615").value->second, 18446744073709551615ull);
}

TEST(RegisteredLineTest, Invalid) {
    for (const char* line : {"SN1,abc", "SN1,", ",123", "SN1,99999999999999999999", "noComma", "SN1,123abc", "SN1,-1"}) {
        EXPECT_FALSE(parseRegisteredDeviceLine(line).ok()) << line;
    }
}

TEST(ParseIntegerTest, BoundaryValuesMatchStoll) {
    EXPECT_EQ(*parseInteger(" 15230").value, 15230);
    EXPECT_EQ(*parseInteger("15230 mW").value, 15230);
    EXPECT_EQ(*parseInteger("-0").value, 0);
    EXPECT_EQ(*parseInteger("\t42").value, 42);
    EXPECT_EQ(*parseInteger("9223372036854775807").value, INT64_MAX);
    EXPECT_EQ(*parseInteger("-9223372036854775808").value, INT64_MIN);
}

TEST(ParseIntegerTest, DoubleSignAndOverflowFail) {
    for (const char* text : {"--5", "+-5", "-+5", " ", "- 5", "-9223372036854775809"}) {
        EXPECT_FALSE(parseInteger(text).ok()) << text;
    }
}
