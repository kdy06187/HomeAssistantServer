#include <limits>
#include <algorithm>
#include "MatterController.hpp"
#include <optional>
#include "ChipToolOutputParser.hpp"
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <ctime>
#include <memory>
#include <array>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

MatterController::MatterController(ChipToolRunner& runner, SetupPinParser parseSetupPin,
                                   std::string chipToolPath, std::string configFilePath,
                                   std::string storageDirectory)
    : mChipToolPath(std::move(chipToolPath)),
      mRunner(runner),
      mParseSetupPin(std::move(parseSetupPin)),
      mConfigFilePath(std::move(configFilePath)),
      mStorageDirectory(std::move(storageDirectory)) {
}

MatterController::~MatterController() {
    shutdown();
}

// 초기화
bool MatterController::Initialize(){
    // chip-tool 저장소 폴더: 없으면 만들고, 있으면 권한을 소유자 전용으로 맞춘다(fabric 키가 들어 있다)
    if (::mkdir(mStorageDirectory.c_str(), 0700) != 0 && errno != EEXIST) {
        std::cerr << "[MatterController] chip-tool 저장소 폴더 생성 실패: " << mStorageDirectory
                  << " (" << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(mStorageDirectory, ec) || ::chmod(mStorageDirectory.c_str(), 0700) != 0) {
        std::cerr << "[MatterController] chip-tool 저장소가 폴더가 아니거나 권한 설정 실패: " << mStorageDirectory << std::endl;
        return false;
    }
    std::vector<std::string> fileLines;
    {
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        std::ifstream file(mConfigFilePath);
        std::string fileLine;
        while (std::getline(file, fileLine)) fileLines.push_back(fileLine);
    }
    int lineNumber = 0;

    for (const std::string& line : fileLines) {
        ++lineNumber;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue; // 빈 줄은 경고 없이 넘긴다.

        auto parsed = ChipToolOutputParser::parseRegisteredDeviceLine(line);
        if (!parsed.ok()) {
            // 줄 내용에 기기 정보가 있을 수 있어 번호만 남긴다.
            std::cerr << "[MatterController] 등록 파일 " << lineNumber << "번째 줄 형식 오류, 건너뜀" << std::endl;
            continue;
        }
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        mSerialToNodeId[parsed.value->first] = parsed.value->second;
    }
    std::cout << "[MatterController] 초기화 완료" << std::endl;
    return true;
}

// 종료
void MatterController::shutdown(){
    std::cout << "[MatterController] 종료" << std::endl;
}

MatterController::ChipToolOutcome MatterController::runChipTool(const std::vector<std::string>& subcommand,
                                                                std::chrono::milliseconds timeout,
                                                                const std::vector<std::string>& secretsToMask,
                                                                bool logDuration) {
    // 셸 없이 인자 배열 그대로 실행한다
    std::vector<std::string> args;
    args.reserve(subcommand.size() + 3);
    args.push_back(mChipToolPath);
    args.insert(args.end(), subcommand.begin(), subcommand.end());
    // 모든 실행이 같은 저장소를 써야 한다. 하나라도 빠지면 그 명령은 /tmp의 다른 fabric을 본다
    args.push_back("--storage-directory");
    args.push_back(mStorageDirectory);
    const auto startedAt = std::chrono::steady_clock::now();
    ProcessResult result = mRunner.run(args, "", timeout, secretsToMask);
    if (logDuration) logChipToolDuration(subcommand, result, std::chrono::steady_clock::now() - startedAt);

    ChipToolOutcome outcome;
    outcome.cancelled = result.cancelled;
    outcome.timedOut = result.timedOut;
    outcome.ok = !result.startFailed && !result.timedOut && !result.cancelled && result.exitCode == 0;
    if (result.startFailed) std::cerr << "[MatterController] chip-tool 실행 실패" << std::endl;
    else if (result.timedOut) std::cerr << "[MatterController] chip-tool 제한 시간 초과" << std::endl;
    else if (result.cancelled) std::cerr << "[MatterController] chip-tool 실행 취소됨" << std::endl;
    else if (result.exitCode != 0) std::cerr << "[MatterController] chip-tool 비정상 종료 (코드 " << result.exitCode << ")" << std::endl;
    outcome.output = std::move(result.output);
    return outcome;
}
void MatterController::logChipToolDuration(const std::vector<std::string>& subcommand, const ProcessResult& result,
                                           std::chrono::steady_clock::duration elapsed) {
    // 하위 명령 앞 두 단어(pairing ble-wifi 등)만 쓴다. 그 뒤 인자에는 비밀값이 있다
    std::string name;
    for (std::size_t i = 0; i < subcommand.size() && i < 2; ++i) name += (i == 0 ? "" : " ") + subcommand[i];
    std::string ending;
    if (result.startFailed) ending = "실행 실패";
    else if (result.timedOut) ending = "제한 시간 초과";
    else if (result.cancelled) ending = "취소";
    else ending = "종료 코드 " + std::to_string(result.exitCode);
    const auto tenths = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 100;
    std::cout << "[MatterController] chip-tool " << name << " 끝: " << ending << ", 걸린 시간 " << tenths / 10 << "."
              << tenths % 10 << "s" << std::endl;
}

std::vector<std::string> MatterController::pairingSecrets(const std::string& wifiSsid, const std::string& wifiPassword,
                                                          const std::string& manualPincode, const std::string& pinText) {
    std::vector<std::string> secrets;
    // 네 값 모두 4바이트 이상만 가린다. 1~3바이트("on" 등)를 ***로 바꾸면 성공 문구까지 깨져 성공한 페어링을
    // 실패로 판정하고, 16진 덤프 등 출력 전체가 망가진다. 빈 값(열린 Wi-Fi 비밀번호)도 여기서 빠진다.
    // SSID는 비밀은 아니지만 집 위치를 특정할 수 있어 가리고, 짧은 SSID는 공개 방송 값이라 그대로 둔다.
    // 짧은 비밀번호·코드·PIN은 가리지 못하므로 실패 출력을 아예 남기지 않는다(hasShortSecret)
    for (const std::string* secret : {&wifiSsid, &wifiPassword, &manualPincode, &pinText}) {
        if (secret->size() >= kMinMaskableSecretBytes) secrets.push_back(*secret);
    }
    return secrets;
}

bool MatterController::hasShortSecret(const std::string& wifiPassword, const std::string& manualPincode,
                                      const std::string& pinText) {
    for (const std::string* secret : {&wifiPassword, &manualPincode, &pinText}) {
        if (!secret->empty() && secret->size() < kMinMaskableSecretBytes) return true;
    }
    return false;
}

std::string MatterController::logTail(const std::string& output) {
    // 실패 원인은 보통 끝에 있다. 출력은 이미 마스킹돼 있어 어디서 잘라도 비밀값 조각이 생기지 않는다
    if (output.size() <= kFailureLogTailBytes) return output;
    return "[앞부분 생략]\n" + output.substr(output.size() - kFailureLogTailBytes);
}

std::string MatterController::runChipToolCommand(const std::vector<std::string>& subcommand, const std::string& deviceId){
    const ChipToolOutcome result = runChipTool(subcommand, kCommandTimeout);
    if (result.cancelled) return "UNKNOWN";  // 기기 상태를 모르는 것이지 오프라인이 아니다
    const std::string& output = result.output;
    if (!result.ok || output.empty() || !ChipToolOutputParser::hasCommandSuccessMarker(output)) {
        std::cerr << "[MatterController] 기기(" << deviceId << ") 통신 에러/오프라인. 사유: "
                  << (result.ok ? "성공 표지 없음" : "실행 실패·시간 초과·비정상 종료") << std::endl;
        markOffline(deviceId); // 오프라인 명부에 추가
        return "OFFLINE";
    }
    auto errorKeyword = ChipToolOutputParser::findCommandErrorKeyword(output);
    if (errorKeyword) {
        std::cerr << "[MatterController] 기기(" << deviceId << ") 통신 에러/오프라인. 사유: " << *errorKeyword << std::endl;
        markOffline(deviceId); // 오프라인 명부에 추가
        // 안드로이드가 쉽게 식별할 수 있도록 OFFLINE을 반환합니다.
        return "OFFLINE";
    }
    // 통신 성공 시 오프라인 명부에서 제거 (온라인 복구)
    if (clearOffline(deviceId)) {
        std::cout << "[MatterController] 기기(" << deviceId << ") 온라인 복구됨." << std::endl;
    }

    return output;
}
// 등록 정보(매핑·파일)는 항상 먼저 정리한다. 반환값은 기기 쪽 해제(chip-tool)의 성공 여부다
bool MatterController::removeDeviceRegistration(const std::string& serialNumber) {
    // 조회와 삭제를 한 락 블록에서 한다. 따로 하면 그 사이 다른 스레드가 매핑을 바꿀 수 있다(TOCTOU)
    std::optional<uint64_t> foundNodeId;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        auto it = mSerialToNodeId.find(serialNumber);
        if (it != mSerialToNodeId.end()) {
            foundNodeId = it->second;
            mSerialToNodeId.erase(it);
            mPairingNodes.erase(*foundNodeId);
            mOfflineNodes.erase(std::to_string(*foundNodeId));
        }
    }

    // 텍스트 파일에서 해당 기기 번호만 삭제 (읽기~덮어쓰기를 한 락으로 묶어 다른 쓰기와 섞이지 않게 한다)
    {
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        std::vector<std::string> kept;
        for (const auto& line : readRegistrationLinesLocked()) {
            const size_t commaPos = line.find(',');
            // 삭제하려는 시리얼 넘버와 다를 때만 남긴다
            if (commaPos != std::string::npos && line.substr(0, commaPos) != serialNumber) kept.push_back(line);
        }
        writeRegistrationLinesLocked(kept);
    }

    if (!foundNodeId) {
        // 매핑이 없으면 노드 번호를 알 수 없다. 0번 노드에 unpair를 보내지 않도록 건너뛴다
        std::cerr << "[MatterController] 시리얼 매핑 없음, 페어링 해제 건너뜀: " << serialNumber << std::endl;
        return false; // 기기 쪽에는 아무것도 보내지 못했다
    }
    const uint64_t deviceId = *foundNodeId;
    const std::string deviceIdStr = std::to_string(deviceId);

    // 2. chip-tool의 내부 KVS 데이터베이스에서도 이별 통보 (unpair)
    std::cout << "[MatterController] 기기(" << deviceIdStr << ") 페어링 해제 중..." << std::endl;
    // 호출한 스레드에서 끝까지 실행한다(최대 kSlowCommandTimeout). 오프라인이어서 타임아웃이 나도 등록 정보는 이미 정리됐다
    const ChipToolOutcome result = runChipTool({"pairing", "unpair", deviceIdStr}, kSlowCommandTimeout, {}, true);
    // 종료 코드 0만 성공으로 본다. 출력 문구는 성공 때도 Error 같은 단어를 담을 수 있어 판정에 쓰지 않는다
    if (!result.ok) {
        std::cerr << "[MatterController] 기기(" << deviceIdStr << ") 해제 명령 실패(오프라인 가능). 로컬에서 강제 페어링 해제 완료." << std::endl;
        return false; // 기기는 아직 이 fabric에 남아 있다(초기화 필요)
    }
    std::cout << "[MatterController] 기기(" << deviceIdStr << ") 정상 페어링 해제 완료" << std::endl;
    return true;
}
bool MatterController::saveDeviceRegistration(const std::string& serialNumber, uint64_t nodeId) {
    std::lock_guard<std::mutex> fileLock(fileMutex_);
    // 같은 시리얼의 옛 줄은 지우고 새 줄로 바꾼다(중복 줄이 쌓이면 시작 때 어느 노드가 맞는지 모호해진다)
    std::vector<std::string> lines;
    for (const auto& line : readRegistrationLinesLocked()) {
        const size_t commaPos = line.find(',');
        if (commaPos != std::string::npos && line.substr(0, commaPos) == serialNumber) continue;
        lines.push_back(line);
    }
    lines.push_back(serialNumber + "," + std::to_string(nodeId));
    return writeRegistrationLinesLocked(lines);
}

std::vector<std::string> MatterController::readRegistrationLinesLocked() {
    std::vector<std::string> lines;
    std::ifstream fileIn(mConfigFilePath);
    std::string line;
    while (std::getline(fileIn, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

bool MatterController::writeRegistrationLinesLocked(const std::vector<std::string>& lines) {
    // 같은 폴더의 임시 파일에 다 쓰고 디스크에 내린 뒤 rename으로 바꿔 끼운다.
    // rename은 원자적이라 중간에 죽어도 "옛 파일 전체" 또는 "새 파일 전체"만 남는다(반쯤 쓴 파일 없음)
    const std::string tempPath = mConfigFilePath + ".tmp";
    std::string content;
    for (const auto& line : lines) content += line + "\n";

    const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::cerr << "[MatterController] 등록 파일 임시본 열기 실패: " << std::strerror(errno) << std::endl;
        return false;
    }
    bool ok = true;
    std::size_t written = 0;
    while (ok && written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false;
        else written += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tempPath.c_str(), mConfigFilePath.c_str()) != 0) ok = false;
    if (!ok) {
        std::cerr << "[MatterController] 등록 파일 쓰기 실패: " << std::strerror(errno) << std::endl;
        ::unlink(tempPath.c_str());
        return false;
    }
    // 이름 바꾸기 자체도 디스크에 남도록 폴더를 동기화한다(실패해도 내용은 이미 맞다)
    const std::string directory = std::filesystem::path(mConfigFilePath).parent_path().string();
    const int dirFd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
    return true;
}
//기기 등록
Result<CommissionOutcome> MatterController::commissionDevice(uint64_t nodeId, const std::string& name,const std::string& manualPincode,
                                const std::string& wifiSsid, const std::string& wifiPassword,
                                const std::atomic<bool>& cancelRequested){
    if (cancelRequested) return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "페어링 전 취소");
    std::cout << "[MatterController] 기기 등록 시도: NodeId=" << nodeId << std::endl;
    const auto setupPin = mParseSetupPin ? mParseSetupPin(manualPincode)
                                         : Result<uint32_t>::failure(ErrorCode::Internal, "설정 코드 해석기 없음");
    if (!setupPin.ok()) {
        return Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "핀 코드 파싱 실패");
    }
    const std::string pinText = std::to_string(*setupPin.value);
   
    // SSID와 비밀번호는 REST 입력이므로 셸을 거치지 않는다.
    // str: 접두사는 hex:로 시작하는 값이 16진수로 디코딩되는 것을 막는다.
    const std::vector<std::string> pairingArgs = {
        "pairing", "ble-wifi", std::to_string(nodeId),
        "str:" + wifiSsid, "str:" + wifiPassword,
        pinText, "3830",
        "--paa-trust-store-path", "../paa_certs/paa-root-certs"
    };

    Result<CommissionOutcome> outcome =
        Result<CommissionOutcome>::failure(ErrorCode::CommissionFailed, "chip-tool 페어링 실패");
    // 2. 에러 스트림 통합 및 결과 읽기. 출력의 비밀값은 러너가 ***로 가린다
    const ChipToolOutcome pairing = runChipTool(pairingArgs, kPairingTimeout,
                                                pairingSecrets(wifiSsid, wifiPassword, manualPincode, pinText), true);
    const std::string& output = pairing.output;

    // 3. 정상 종료이고 성공 키워드가 있을 때만 성공 (chip-tool 페어링 성공 로그 기준)
    // 종료 때 cancelAll이 페어링을 끊었거나, 페어링 직후 취소됐으면 시리얼 읽기·매핑 저장 없이 끝낸다
    if (pairing.cancelled || cancelRequested) {
        std::cerr << "[MatterController] 커미셔닝 취소됨 (NodeId=" << nodeId << ")" << std::endl;
        return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "페어링 취소");
    }
    if (pairing.ok && ChipToolOutputParser::isCommissioningSuccessful(output)) {
        const auto serialNumber = this->onDevicePairingComplete(nodeId, name, cancelRequested);
        if (!serialNumber) {
            // 종료 중이라 chip-tool unpair도 거부된다. 기기 쪽 fabric에는 이 노드가 남는다(다음 등록 전 기기 초기화)
            std::cerr << "[MatterController] 시리얼 읽기 중 취소 — 등록하지 않음, 기기 fabric에 노드 " << nodeId
                      << "가 남을 수 있음(다시 등록하려면 기기 초기화)" << std::endl;
            return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "시리얼 읽기 취소");
        }
        // 매핑·등록 파일은 서버 등록이 확정된 뒤 finalizeCommission에서 쓴다
        std::cout << "[MatterController] 커미셔닝 성공 (NodeId=" << nodeId << "), 서버 등록 대기" << std::endl;
        outcome = Result<CommissionOutcome>::success(CommissionOutcome{*serialNumber, "", nodeId});
    } else {
        if (pairing.timedOut) {
            outcome = Result<CommissionOutcome>::failure(ErrorCode::CommissionTimeout, "chip-tool 페어링 시간 초과");
        }
        // 4바이트 미만 비밀값은 가리지 않으므로(위 pairingSecrets) 그런 값이 있으면 출력을 아예 남기지 않는다
        if (hasShortSecret(wifiPassword, manualPincode, pinText)) {
            std::cerr << "[MatterController] 커미셔닝 실패 (타임아웃 또는 에러): 출력 생략: 짧은 비밀값" << std::endl;
        } else {
            std::cerr << "[MatterController] 커미셔닝 실패 (타임아웃 또는 에러): \n" << logTail(output) << std::endl;
        }
    }

    return outcome;  // "페어링 중" 표시는 호출자의 PairingGuard가 푼다
}

std::optional<std::string> MatterController::getDeviceSerialNumber(uint64_t nodeId, const std::atomic<bool>& cancelRequested) {
    std::cout << "[MatterController] 기기 시리얼 넘버 조회 중... (NodeID: " << nodeId << ")" << std::endl;
    
    // chip-tool basic 클러스터 사용
    const ChipToolOutcome result = runChipTool({"basicinformation", "read", "serial-number", std::to_string(nodeId), "0"},
                                               kSlowCommandTimeout, {}, true);

    // 취소된 읽기를 "시리얼 없음"으로 보면 UNKNOWN_SERIAL_<노드>로 유령 기기가 등록된다
    if (result.cancelled || cancelRequested) return std::nullopt;
    if (result.ok) {
        auto serial = ChipToolOutputParser::extractSerialNumber(result.output);
        if (serial) return *serial;
    }

    // 시리얼 넘버를 지원하지 않는 기기일 경우, 안전장치로 NodeId 기반의 문자열 반환
    return "UNKNOWN_SERIAL_" + std::to_string(nodeId);
}

// 기기 제어 Turn on
bool MatterController::turnOn(uint64_t nodeId,uint16_t endpointId){
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] 기기 페어링 중... 켜기 요청 스킵." << std::endl;
        return false;
    }
    std::cout << "[MatterController] 💡 기기 켜기 요청 (NodeID: " << nodeId << ")" << std::endl;

    std::string output = runChipToolCommand({"onoff", "on", std::to_string(nodeId), std::to_string(endpointId)}, std::to_string(nodeId));
    if (output == "OFFLINE" || output == "UNKNOWN") {
        return false;
    }

    return true;
}

// 기기 제어 Turn off
bool MatterController::turnOff(uint64_t nodeId,uint16_t endpointId){
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] ⏳ 기기 페어링 중. 끄기 요청 스킵." << std::endl;
        return false;
    }
    std::cout << "[MatterController] 🔌 기기 끄기 요청 (NodeID: " << nodeId << ")" << std::endl;

    std::string output = runChipToolCommand({"onoff", "off", std::to_string(nodeId), std::to_string(endpointId)}, std::to_string(nodeId));
    if (output == "OFFLINE" || output == "UNKNOWN") {
        return false;
    }

    return true;
}

// 인터페이스 구현
bool MatterController::sendCommand(const std::string& deviceId, const std::string& command){
    
    uint64_t nodeId = getNodeId(deviceId);
    if (nodeId == 0) {
        // 매핑이 없으면 0번 노드에 명령을 보내게 되므로 실행하지 않는다
        std::cerr << "[MatterController] 시리얼 매핑 없음, 명령 건너뜀: " << deviceId << std::endl;
        return false;
    }

    if(command == "ON"|| command == "TURN_ON"){
        return turnOn(nodeId,1);
    } else if(command == "OFF"|| command == "TURN_OFF"){
        return turnOff(nodeId,1);
    }
    return false;
}
// Matter 기기 ID(시리얼)는 페어링 뒤에야 알 수 있어 활성 ID 목록은 쓰지 않는다
Result<CommissionOutcome> MatterController::commission(const CommissionRequest& request,
                                                       const std::unordered_set<std::string>& /*activeDeviceIds*/,
                                                       const std::atomic<bool>& cancelRequested){
    // 종료 중이면 노드 번호를 쓰지 않고 바로 끝낸다
    if (cancelRequested) return Result<CommissionOutcome>::failure(ErrorCode::Cancelled, "등록 전 취소");
    const uint64_t nodeId = reserveNodeIdForPairing();
    PairingGuard pairingGuard(*this, nodeId);
    return commissionDevice(nodeId, request.name, request.payload, request.ssid, request.password, cancelRequested);
}
std::optional<std::string> MatterController::onDevicePairingComplete(uint64_t nodeId, const std::string& deviceName,
                                                                     const std::atomic<bool>& cancelRequested) {
    std::cout << "[MatterController] 기기 페어링 완료: NodeId=" << nodeId << ", DeviceName=" << deviceName << std::endl;
    return getDeviceSerialNumber(nodeId, cancelRequested);
}
void MatterController::finalizeCommission(const CommissionOutcome& outcome, bool accepted) {
    if (outcome.nodeId == 0) return;
    const std::string nodeIdText = std::to_string(outcome.nodeId);
    if (accepted) {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            mSerialToNodeId[outcome.deviceId] = outcome.nodeId;
        }
        if (!saveDeviceRegistration(outcome.deviceId, outcome.nodeId)) {
            std::cerr << "[MatterController] 등록 파일 저장 실패: 재시작하면 이 기기의 노드 매핑이 사라집니다 (" << outcome.deviceId << ")" << std::endl;
        }
        return;
    }
    // 서버가 등록하지 않았다: 이번에 만든 노드만 fabric에서 뺀다. 같은 시리얼의 기존 매핑(활성 기기)은 건드리지 않는다
    std::cout << "[MatterController] 등록되지 않은 새 노드(" << nodeIdText << ") 페어링 해제 중..." << std::endl;
    const ChipToolOutcome result = runChipTool({"pairing", "unpair", nodeIdText}, kSlowCommandTimeout, {}, true);
    if (!result.ok) {
        std::cerr << "[MatterController] 새 노드(" << nodeIdText << ") 해제 실패 — 기기 fabric에 남을 수 있음(다시 등록하려면 기기 초기화)" << std::endl;
    }
}
bool MatterController::unpairDevice(const std::string& deviceId){
    std::cout << "[MatterController] Matter 기기 페어링 해제 : NodeId = " << deviceId << std::endl;
    bool success = this->removeDeviceRegistration(deviceId);
    return success;
}
std::string MatterController::readDeviceState(const std::string& rawDeviceId, bool isManualRequest){
    std::string deviceId = rawDeviceId;
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\n'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\r'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\"'), deviceId.end());
    deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), ' '), deviceId.end());

    uint64_t nodeId = getNodeId(deviceId);
    if (nodeId == 0) {
        std::cerr << "[MatterController] 시리얼 매핑 없음, 상태 읽기 건너뜀: " << deviceId << std::endl;
        return "UNKNOWN";  // 상태를 모름(기기를 오프라인으로 바꾸지 않는다)
    }

    // 기기가 현재 페어링 중이면 즉시 스킵
    if (isPairing(nodeId)) {
        std::cout << "[MatterController] 기기(" << nodeId << ") 페어링 중. 헬스체크 스킵." << std::endl;
        return "PAIRING";
    }

    // 자동 헬스체크인데 기기가 오프라인 명부에 있다면 쉘 명령 실행 스킵
    if (!isManualRequest && isOffline(std::to_string(nodeId))) {
        std::cout << "[MatterController] 기기(" << nodeId << ") 오프라인 상태. 헬스체크 스킵." << std::endl;
        return "OFFLINE";
    }

    std::cout << "[MatterController] 기기 상태 읽기 요청 : NodeId = " << nodeId << std::endl;

    try{
        std::string output = this->runChipToolCommand({"onoff", "read", "on-off", std::to_string(nodeId), "1"}, std::to_string(nodeId));
        
        if (output == "OFFLINE" || output == "UNKNOWN") return output;

        auto onOffState = ChipToolOutputParser::parseOnOffState(output);
        if (onOffState) return *onOffState;
    }catch(const std::exception& e){
        std::cerr << "[MatterController] 상태 읽기 실패: " << e.what() << std::endl;
        return "ERROR";
    }
    return "UNKNOWN";
}
// 값 해석 규칙은 ChipToolOutputParser::parseEnergyReading에 있다
Result<EnergyReading> MatterController::readEnergy(const std::string& deviceId){
    // 매핑을 한 번만 읽는다. 두 조회 사이에 삭제돼도 노드 0에 chip-tool을 실행하지 않는다
    const uint64_t nodeId = getNodeId(deviceId);
    if (nodeId == 0) {
        return Result<EnergyReading>::failure(ErrorCode::DeviceNotFound, "시리얼 매핑 없음");
    }
    const std::string activePowerText = readActivePowerText(nodeId, false);
    // 첫 조회에서 닿지 못했으면 두 번째 chip-tool 실행(최대 5초)을 생략한다
    if (ChipToolOutputParser::isUnreachableMarker(activePowerText)) {
        return ChipToolOutputParser::parseEnergyReading(activePowerText, "");
    }
    const std::string cumulativeText = readCumulativeEnergyText(nodeId, false);
    return ChipToolOutputParser::parseEnergyReading(activePowerText, cumulativeText);
}
std::string MatterController::getPowerUsage(std::string deviceId, bool isManualRequest){
    return readActivePowerText(getNodeId(deviceId), isManualRequest);
}
std::string MatterController::getCumulativeEnergy(std::string deviceId, bool isManualRequest){
    return readCumulativeEnergyText(getNodeId(deviceId), isManualRequest);
}
std::string MatterController::readActivePowerText(uint64_t nodeId, bool isManualRequest){
    // 기기가 현재 페어링 중이면 스킵
    if (isPairing(nodeId)) return "PAIRING";

    // 오프라인 기기의 자동 조회 스킵
    if (!isManualRequest && isOffline(std::to_string(nodeId))) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(실시간) 조회 요청 : NodeId = " << nodeId << std::endl;
    std::string output = this->runChipToolCommand(
        {"electricalpowermeasurement", "read", "active-power", std::to_string(nodeId), "1"}, std::to_string(nodeId));

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = ChipToolOutputParser::extractActivePowerText(output).value_or("UNKNOWN");

    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 실시간 전력 : " << result << " mW" << std::endl;
    }
    return result;
}
std::string MatterController::readCumulativeEnergyText(uint64_t nodeIdValue, bool isManualRequest){
    std::string nodeId = std::to_string(nodeIdValue);

    // getPowerUsage와 같은 건너뛰기 규칙
    if (isPairing(nodeIdValue)) return "PAIRING";
    if (!isManualRequest && isOffline(nodeId)) return "OFFLINE";

    std::cout << "[MatterController] 기기 전력량(누적) 조회 요청 : NodeId = " << nodeId << std::endl;
    // 러너가 stderr도 함께 읽으므로(셸의 2>&1과 같음) 오류가 stderr로만 나와도 판정에 쓰인다
    std::string output = this->runChipToolCommand(
        {"electricalenergymeasurement", "read", "cumulative-energy-imported", nodeId, "1"}, nodeId);

    if (output == "OFFLINE" || output == "UNKNOWN") return output;

    if (output.empty()) return "UNKNOWN";

    std::string result = ChipToolOutputParser::extractEnergyText(output).value_or("UNKNOWN");
    if (result != "UNKNOWN") {
        std::cout << "[MatterController] 누적 전력 : " << result << " mWh" << std::endl;
    }
    return result;
}
std::uint64_t MatterController::getNodeId(const std::string& serialNumber) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    auto it = mSerialToNodeId.find(serialNumber);
    if (it != mSerialToNodeId.end()) {
        return it->second;
    }
    return 0;
}

// ---- 상태 명부 접근: 모두 stateMutex_ 안에서 메모리만 다룬다 ----
bool MatterController::isPairing(uint64_t nodeId) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mPairingNodes.count(nodeId) > 0;
}
void MatterController::setPairing(uint64_t nodeId, bool pairing) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (pairing) mPairingNodes.insert(nodeId);
    else mPairingNodes.erase(nodeId);
}
bool MatterController::isOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mOfflineNodes.count(nodeIdText) > 0;
}
void MatterController::markOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    mOfflineNodes[nodeIdText] = std::chrono::steady_clock::now();
}
bool MatterController::clearOffline(const std::string& nodeIdText) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return mOfflineNodes.erase(nodeIdText) > 0;
}

uint64_t MatterController::reserveNodeIdForPairing() {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    uint64_t candidate = std::max<uint64_t>(static_cast<uint64_t>(std::time(nullptr)), mLastIssuedNodeId + 1);
    auto inUse = [this](uint64_t id) {
        if (mPairingNodes.count(id) > 0) return true;
        for (const auto& pair : mSerialToNodeId) if (pair.second == id) return true;
        return false;
    };
    while (inUse(candidate)) ++candidate;
    mLastIssuedNodeId = candidate;
    mPairingNodes.insert(candidate);
    return candidate;
}
