#pragma once

#include <atomic>

namespace loggable {
namespace espidf {

/**
 * @brief ESP-IDF platform adapter for the loggable library.
 *
 * This class provides the ESP-IDF specific log hooking functionality,
 * allowing logs made via ESP_LOGx macros to be captured and dispatched
 * through the loggable Sinker.
 */
class LogHook {
public:
    LogHook() = delete;
    LogHook(const LogHook&) = delete;
    LogHook& operator=(const LogHook&) = delete;

    /**
     * @brief Install the ESP-IDF log hook.
     *
     * When installed, all logs made via `ESP_LOGx` macros will be redirected
     * through the loggable Sinker.
     */
    static void install() noexcept;

    /**
     * @brief Uninstall the ESP-IDF log hook.
     *
     * Restores the original vprintf handler.
     */
    static void uninstall() noexcept;

    /**
     * @brief Check if the hook is currently installed.
     * @return true if the hook is installed, false otherwise.
     */
    [[nodiscard]] static bool is_installed() noexcept;

private:
    static std::atomic<bool> _installed;
};

} // namespace espidf
} // namespace loggable
