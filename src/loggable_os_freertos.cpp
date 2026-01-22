#include "loggable_os.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace loggable {
namespace os {

/**
 * @brief FreeRTOS implementation of IAsyncBackend.
 *
 * This is a singleton that provides async logging support on ESP-IDF/FreeRTOS.
 * Register with set_backend(&FreeRTOSBackend::instance()) before Sinker::init().
 */
class FreeRTOSBackend final : public IAsyncBackend {
public:
    /**
     * @brief Get the singleton instance.
     */
    static FreeRTOSBackend &instance() noexcept {
        static FreeRTOSBackend inst;
        return inst;
    }

    FreeRTOSBackend(const FreeRTOSBackend &) = delete;
    FreeRTOSBackend &operator=(const FreeRTOSBackend &) = delete;

    SemaphoreHandle semaphore_create_binary() noexcept override {
        return SemaphoreHandle{xSemaphoreCreateBinary()};
    }

    void semaphore_destroy(SemaphoreHandle sem) noexcept override {
        if (sem) {
            vSemaphoreDelete(static_cast<::SemaphoreHandle_t>(sem._handle));
        }
    }

    void semaphore_give(SemaphoreHandle sem) noexcept override {
        if (sem) {
            xSemaphoreGive(static_cast<::SemaphoreHandle_t>(sem._handle));
        }
    }

    bool semaphore_take(SemaphoreHandle sem, uint32_t timeout_ms) noexcept override {
        if (!sem) {
            return false;
        }

        const TickType_t ticks =
            (timeout_ms == WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xSemaphoreTake(static_cast<::SemaphoreHandle_t>(sem._handle),
                              ticks) == pdTRUE;
    }

    TaskHandle task_create(const TaskConfig &config,
                           TaskFunction fn,
                           void *arg) noexcept override {
        ::TaskHandle_t handle = nullptr;
        BaseType_t result;

        if (config.core >= 0) {
            result = xTaskCreatePinnedToCore(fn,
                                             config.name,
                                             config.stack_size,
                                             arg,
                                             config.priority,
                                             &handle,
                                             config.core);
        } else {
            result = xTaskCreate(fn,
                                 config.name,
                                 config.stack_size,
                                 arg,
                                 config.priority,
                                 &handle);
        }

        return TaskHandle{(result == pdPASS) ? handle : nullptr};
    }

    void task_delete(TaskHandle task) noexcept override {
        vTaskDelete(static_cast<::TaskHandle_t>(task._handle));
    }

    void delay_ms(uint32_t ms) noexcept override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

private:
    FreeRTOSBackend() = default;
};

IAsyncBackend &get_freertos_backend() noexcept {
    return FreeRTOSBackend::instance();
}

} // namespace os
} // namespace loggable
