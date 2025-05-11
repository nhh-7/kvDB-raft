#include "./include/utils.hpp"

namespace monsoon
{
    pid_t GetThreadId()
    {
        return syscall(SYS_gettid);
    }

    uint32_t GetFiberId()
    {
        // 此处功能未完成
        return 0;
    }
}