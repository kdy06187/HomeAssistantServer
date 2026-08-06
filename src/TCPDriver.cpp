#include "TCPDriver.hpp"
#include <iostream>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

TCPDriver::TCPDriver() : running_(false) {
    std::cout << "[TCPDriver] 인스턴스 생성" << std::endl;
}

TCPDriver::~TCPDriver(){
    stopServer();
    std::cout << "[TCPDriver] 인스턴스 소멸" << std::endl;
}

bool TCPDriver::sendCommand(std::string device_id, std::string command){
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    auto it = client_sockets_.find(device_id);
    if(it != client_sockets_.end()){
        int client_socket = it->second;
        // Send the command to the client

        std::string payload = command + "\n"; // 명령어 끝에 개행 추가
        // send 함수를 사용하여 명령어 전송
        ssize_t bytes_sent = send(client_socket,payload.c_str(), payload.length(),0);

        if(bytes_sent < 0){
            std::cerr << "[TCPDriver] 에러 : " << device_id << " 기기로 명령 전송 실패" << std::endl;
        } else {
            std::cout << "[TCPDriver] " << device_id << " 기기로 명령 전송 성공 : " << command << std::endl;

        }
    } else{
        std::cerr << "[TCPDriver] 에러 : " << device_id << " 기기와 연결되어 있지 않습니다." << std::endl;
    }
}

void TCPDriver::startServer(int port){
    if(running_) return; // 이미 서버가 실행 중이면 무시
    running_ = true;
    // 서버를 별도의 스레드에서 실행
    server_thread_ = std::thread(&TCPDriver::accecptLoop, this,port);
    std::cout << "[TCPDriver] TCP 서버 시작 ( 포트 : " << port << ")" << std::endl;
}

void TCPDriver::stopServer(){
    running_ = false;
    if(server_thread_.joinable()){
        server_thread_.join();
    }
    std::cout << "[TCPDriver] TCP 서버 종료" << std::endl;
}

void TCPDriver::accecptLoop(int port){
    // TCP 서버 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        std::cerr << "[TCPDriver] 에러 : 서버 소켓 생성 실패" << std::endl;
        return;
    }
    // 포트 충돌 방지 옵션
    int opt = 1;
    setsockopt(server_fd,SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    //bind 포트와 소켓 결합
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 모든 IP의 접속 허용
    address.sin_port = htons(port); // 포트 번호 지정

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        std::cerr << "[TCPDriver] 에러 : 포트 " << port << " 바인딩 실패" << std::endl;
        close(server_fd);
        return;
    }
    //listen (대기열 생성)
    if(listen(server_fd,10) < 0){
        std::cerr << "[TCPDriver] 에러 : 포트 " << port << " 리슨 실패" << std::endl;
        close(server_fd);
        return;
    }

    std::cout << "[TCPDriver] " << port << " 포트에서 클라이언트 연결 대기 중..." << std::endl;
    while(running_){
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        //기기 접속 대기
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen); // 클라이언트 연결 수락 - 전용 1:1 통신 채널 파이프 
        if(client_fd < 0){
            std::cerr << "[TCPDriver] 에러 : 클라이언트 연결 수락 실패" << std::endl;
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        std::string client_ip(ip_str);

        std::cout << "[TCPDriver] 클라이언트 연결 수락 IP : " << client_ip << std::endl;
        
        // Json 형식으로 기기 ID를 받지만 임시로 지정
        std::string device_id = "Arduino_1"; // 임시로 지정
        {
            std::lock_guard<std::mutex> lock(sockets_mutex_);
            client_sockets_[device_id] = client_fd; // 기기 ID와 소켓 번호 저장
        }
        std::cout << "[TCPDriver] 기기 ID [" << device_id << "] 와 소켓이 매핑되었습니다." << std::endl;
    }
    close(server_fd);
}