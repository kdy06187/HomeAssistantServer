#pragma once
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>

// std::cout·std::cerr 출력을 모아 시험이 비밀값이 찍혔는지 볼 수 있게 한다.
// 여러 스레드(HTTP·작업 스레드)가 동시에 써도 되도록 버퍼를 락으로 지킨다.
// 바꿔 끼우기·되돌리기는 출력하는 스레드가 없을 때만 한다(스레드 시작 전 start, join 뒤 stop)
class LogCapture {
public:
    ~LogCapture() { stop(); }
    void start() {
        if (active_) return;
        oldOut_ = std::cout.rdbuf(&buffer_);
        oldErr_ = std::cerr.rdbuf(&buffer_);
        active_ = true;
    }
    void stop() {
        if (!active_) return;
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
        active_ = false;
    }
    std::string text() { return buffer_.text(); }

private:
    class LockedBuffer : public std::streambuf {
    public:
        std::string text() {
            std::lock_guard<std::mutex> lock(mutex_);
            return data_;
        }

    protected:
        int_type overflow(int_type ch) override {
            if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
            std::lock_guard<std::mutex> lock(mutex_);
            data_.push_back(traits_type::to_char_type(ch));
            return ch;
        }
        std::streamsize xsputn(const char* text, std::streamsize count) override {
            std::lock_guard<std::mutex> lock(mutex_);
            data_.append(text, static_cast<std::size_t>(count));
            return count;
        }

    private:
        std::mutex mutex_;
        std::string data_;
    };

    LockedBuffer buffer_;
    std::streambuf* oldOut_ = nullptr;
    std::streambuf* oldErr_ = nullptr;
    bool active_ = false;
};
