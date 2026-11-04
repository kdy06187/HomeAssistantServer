#pragma once
#include <string>
#include <thread>
#include <atomic>
class DeviceManager;

class HTTPServer{
public:
    HTTPServer(DeviceManager& deviceManager, std::string apiToken, int port = 8000);
    ~HTTPServer();

    bool start();
    void stop();

private:
DeviceManager& mDeviceManager;
    std::string mApiToken;
    int mPort;
    std::atomic<bool> mIsRunning;
    std::thread mServerThread;

    void run();

    std::string handleGetDevices();
    std::string handleControlDevice(const std::string& requestBody);
    std::string handleCommissionDevice(const std::string& requestBody);
    std::string handleDeleteDevice(const std::string& requestBody);
    std::string handleGetDeviceEnergy(const std::string& deviceId);
    std::string handleGetDeviceEnergyHistory(const std::string& deviceId);
};
