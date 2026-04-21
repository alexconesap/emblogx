// Stub for SystemControl used by OTA updater tests
#include <system/system_reboot.h>

namespace ungula {

    void SystemControl::reboot() {}
    void SystemControl::rebootAfterMs(uint32_t) {}

}  // namespace ungula
