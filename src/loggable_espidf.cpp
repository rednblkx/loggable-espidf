#include "loggable_espidf.hpp"
#include "loggable.hpp"
#include <esp_log.h>
#include <mutex>
#include <string>
#include <string_view>
#include <charconv>
#include <cstdarg>
#include <cstdio>

namespace loggable {
namespace espidf {

namespace {

static vprintf_like_t original_vprintf = nullptr;
static std::mutex hook_mutex;

struct ThreadBufferState {
    std::string log_buffer;
};

static ThreadBufferState& get_thread_buffer() {
    static thread_local ThreadBufferState buffer_state;
    return buffer_state;
}

void cleanup_message(std::string& message) {
    if (message.find("\033[") != std::string::npos) {
        size_t start_pos = 0;
        while ((start_pos = message.find("\033[", start_pos)) != std::string::npos) {
            size_t end_pos = message.find('m', start_pos);
            if (end_pos == std::string::npos) {
                break;
            }
            message.erase(start_pos, end_pos - start_pos + 1);
        }
    }

    if (!message.empty() && message.back() == '\n') {
        message.pop_back();
    }
}

void dispatch_to_sinker(std::string_view message) {
    LogLevel level = LogLevel::Info;
    std::string tag;  // Empty by default
    std::string payload;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();

    // A typical ESP-IDF log looks like: "L (TIME) TAG: MESSAGE"
    if (message.length() > 4 && message[1] == ' ' && (message[0] == 'E' || message[0] == 'W' || message[0] == 'I' || message[0] == 'D' || message[0] == 'V')) {
        switch(message[0]) {
            case 'E': level = LogLevel::Error; break;
            case 'W': level = LogLevel::Warning; break;
            case 'I': level = LogLevel::Info; break;
            case 'D': level = LogLevel::Debug; break;
            case 'V': level = LogLevel::Verbose; break;
            default: break;
        }

        const size_t tag_start = message.find('(');
        const size_t tag_end = message.find(')', tag_start);
        const size_t payload_start = message.find(':', tag_end);

        if (tag_end != std::string_view::npos && payload_start != std::string_view::npos && tag_end + 1 < message.length()) {
            if (tag_start != std::string_view::npos && tag_end > tag_start + 1) {
                const size_t timestamp_start = tag_start + 1;
                const size_t timestamp_length = tag_end - timestamp_start;
                std::string_view timestamp_str = message.substr(timestamp_start, timestamp_length);
                
                unsigned long millis_since_boot = 0;
                auto [ptr, ec] = std::from_chars(timestamp_str.data(), timestamp_str.data() + timestamp_str.size(), millis_since_boot);
                
                if (ec == std::errc()) {
                    timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(millis_since_boot));
                }
            }
            
            if (message[tag_end + 1] == ' ') {
                const size_t tag_text_start = tag_end + 2;
                if (tag_text_start < payload_start) {
                    tag = std::string(message.substr(tag_text_start, payload_start - tag_text_start));
                }
            }

            const size_t payload_text_start = payload_start + 2;
            if (payload_text_start < message.length()) {
                payload = std::string(message.substr(payload_text_start));
            } else {
                payload = "";
            }
        } else {
            payload = std::string(message);
        }
    } else {
        payload = std::string(message);
    }
    
    LogMessage log_msg(timestamp, level, std::move(tag), std::move(payload));
    Sinker::instance().dispatch(log_msg);
}

int vprintf_hook(const char* format, va_list args) {
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        original_vprintf(format, args_copy);
        va_end(args_copy);
    }

    thread_local bool is_logging = false;
    if (is_logging) {
        return 0;
    }
    is_logging = true;
    struct LoggingGuard {
        bool& flag;
        ~LoggingGuard() { flag = false; }
    } guard{is_logging};

    char static_buf[128];
    va_list args_copy;
    va_copy(args_copy, args);
    int size = std::vsnprintf(static_buf, sizeof(static_buf), format, args_copy);
    va_end(args_copy);

    if (size < 0) [[unlikely]] {
        return 0;
    }
    
    std::string_view formatted_message_view(static_buf, size);
    
    std::string dynamic_message;
    if (static_cast<size_t>(size) >= sizeof(static_buf)) {
        dynamic_message.resize(size);
        va_copy(args_copy, args);
        std::vsnprintf(dynamic_message.data(), dynamic_message.size() + 1, format, args_copy);
        va_end(args_copy);
        formatted_message_view = dynamic_message;
    }
    
    auto& buffer_state = get_thread_buffer();

    buffer_state.log_buffer.append(formatted_message_view);

    if (!buffer_state.log_buffer.empty() && buffer_state.log_buffer.back() == '\n') {
        std::string complete_message = std::move(buffer_state.log_buffer);
        
        buffer_state.log_buffer.clear();
        
        cleanup_message(complete_message);
        if (!complete_message.empty()) {
            dispatch_to_sinker(complete_message);
        }
    }
    return size;
}

}

std::atomic<bool> LogHook::_installed{false};

void LogHook::install() noexcept {
    std::lock_guard<std::mutex> lock(hook_mutex);
    if (!_installed.load(std::memory_order_acquire)) {
        original_vprintf = esp_log_set_vprintf(&vprintf_hook);
        _installed.store(true, std::memory_order_release);
    }
}

void LogHook::uninstall() noexcept {
    std::lock_guard<std::mutex> lock(hook_mutex);
    if (_installed.load(std::memory_order_acquire)) {
        esp_log_set_vprintf(original_vprintf);
        original_vprintf = nullptr;
        _installed.store(false, std::memory_order_release);
    }
}

bool LogHook::is_installed() noexcept {
    return _installed.load(std::memory_order_acquire);
}

} // namespace espidf
} // namespace loggable
