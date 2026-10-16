// gow3: GPU-side assertion failures stop the port with exit code 23, after the stop hook (the
// runtime's play-test report, src/runtime_report.c) has recorded where.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"

namespace {
void (*stop_hook)(const char*) = nullptr;

[[noreturn]] void Stop(const char* why) {
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    if (stop_hook) {
        stop_hook(why);
    }
    std::_Exit(23);
}
} // namespace

extern "C" void gow3gpu_set_stop_hook(void (*hook)(const char* why)) {
    stop_hook = hook;
}

void assert_fail_impl() {
    Stop("A check in the GPU library failed; its message is at the end of log-tail.txt.");
}

[[noreturn]] void unreachable_impl() {
    Stop("The GPU library reached code it treats as unreachable (\"Unreachable code\" in the log).");
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    static char why[512];
    std::snprintf(why, sizeof(why), "A check in the GPU library failed: %s", msg ? msg : "?");
    Stop(why);
}
