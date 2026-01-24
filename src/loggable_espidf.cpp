#include "loggable_espidf.hpp"
#include "loggable.hpp"
#include "loggable_os.hpp"
#include <esp_log.h>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace loggable {

namespace os {
IAsyncBackend& get_freertos_backend() noexcept;
} // namespace os

namespace espidf {

namespace {

static bool _call_original_vprintf = true;
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

        const size_t time_start = message.find('(');
        const size_t time_end = message.find(')', time_start);
        size_t message_start = message.find(':', time_end);
        if((message_start + 1) == ':') message_start = message.find(':', message_start + 1);

        if (time_end != std::string_view::npos && message_start != std::string_view::npos && time_end + 1 < message.length()) {
            if (time_start != std::string_view::npos && time_end > time_start + 1) {
                const size_t timestamp_start = time_start + 1;
                const size_t timestamp_length = time_end - timestamp_start;
                std::string_view timestamp_str = message.substr(timestamp_start, timestamp_length);
                
                unsigned long millis_since_boot = 0;
                auto [ptr, ec] = std::from_chars(timestamp_str.data(), timestamp_str.data() + timestamp_str.size(), millis_since_boot);
                
                if (ec == std::errc()) {
                    timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(millis_since_boot));
                }
            }
            
            if (message[time_end + 1] == ' ') {
                const size_t tag_text_start = time_end + 2;
                if (tag_text_start < message_start) {
                    tag = std::string(message.substr(tag_text_start, message_start - tag_text_start));
                }
            }

            if (message_start < message.length()) {
                payload = std::string(message.substr(message[message_start + 1] == ' ' ? message_start + 2 : message_start + 1));
            } else {
                payload = "";
            }
        } else {
            payload = std::string(message);
        }
    } else {
        payload = std::string(message);
    }
    
    Sinker::instance().dispatch(LogMessage{timestamp, level, std::move(tag), std::move(payload)});
}

int vprintf_hook(const char* format, va_list args) {
    if (original_vprintf && _call_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        original_vprintf(format, args_copy);
        va_end(args_copy);
    }

    #if defined(CONFIG_IDF_TARGET_ESP32C3)
    // ESP32-C3 either has broken TLS or some strange config option that needs tweaking i haven't found, use static instead (safe on single-core)
    static bool is_logging = false;
    #else
    thread_local bool is_logging = false;
    #endif
    if (is_logging) {
        return 0;
    }
    is_logging = true;
    struct LoggingGuard {
        bool& flag;
        ~LoggingGuard() { flag = false; }
    } guard{is_logging};

    char static_buf[256];
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

void LogHook::install(bool call_original_vprintf) noexcept {
    std::lock_guard<std::mutex> lock(hook_mutex);
    _call_original_vprintf = call_original_vprintf;
    if (!_installed.load(std::memory_order_acquire)) {
        os::set_backend(&os::get_freertos_backend());

        Sinker::instance().init();

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

        Sinker::instance().shutdown();
    }
}

bool LogHook::is_installed() noexcept {
    return _installed.load(std::memory_order_acquire);
}

} // namespace espidf
} // namespace loggable
