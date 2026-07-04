#include "GameTaskQueue.h"
#include "Log.h"

namespace sow {
namespace {

constexpr int kQueueCap = 128;

struct Task {
    HagUI_GameTaskFn fn = nullptr;
    void* ctx = nullptr;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Task g_queue[kQueueCap]{};
int g_head = 0;
int g_tail = 0;
int g_count = 0;

bool PopTask(Task& out) {
    ::AcquireSRWLockExclusive(&g_lock);
    if (g_count == 0) {
        ::ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    out = g_queue[g_head];
    g_queue[g_head] = {};
    g_head = (g_head + 1) % kQueueCap;
    --g_count;
    ::ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool CallTaskSeh(HagUI_GameTaskFn fn, void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool QueueGameTask(HagUI_GameTaskFn fn, void* ctx) {
    if (!fn) return false;
    ::AcquireSRWLockExclusive(&g_lock);
    if (g_count >= kQueueCap) {
        ::ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_queue[g_tail] = { fn, ctx };
    g_tail = (g_tail + 1) % kQueueCap;
    ++g_count;
    ::ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void DrainGameTasks(int maxTasks) {
    if (maxTasks <= 0) return;
    for (int i = 0; i < maxTasks; ++i) {
        Task t{};
        if (!PopTask(t)) return;
        if (!CallTaskSeh(t.fn, t.ctx)) {
            Log::Get().Line("[gametask] queued callback raised SEH");
        }
    }
}

}  // namespace sow
