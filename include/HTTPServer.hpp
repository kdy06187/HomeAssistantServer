#pragma once
#include <string>
#include <thread>
#include <atomic>
class DeviceManager;

class HTTPServer{
public:
    HTTPServer(DeviceManager& deviceManager,int port = 8000);
    ~HTTPServer();

    bool start();
    void stop();

private:
DeviceManager& mDeviceManager;
    int mPort;
    std::atomic<bool> mIsRunning;
    std::thread mServerThread;

    void run();

    std::string handleGetDevices();
    std::string handleControlDevice(const std::string& requestBody);
};
