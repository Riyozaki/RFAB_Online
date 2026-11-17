// The smallest program that uses the server's crash record (Code/server_runner/CrashRecord.cpp) the
// way the server executable does. Tools/rfab/linux-server/crash-record-self-test.sh builds and runs
// it: the part of the record that is portable - std::terminate with the exception's text - is
// checked on Linux in every run; the Windows part (access violations, call stacks) is checked by the
// release pipeline with the real executable (Tools/rfab/Package-Release.ps1).

#include "CrashRecord.h"

#include <cstring>

namespace
{
void FaultHere()
{
    volatile int* volatile pNowhere = nullptr;
    *pNowhere = 1;
}
} // namespace

int main(int argc, char** argv)
{
    ServerCrash::Install();
    ServerCrash::Install();  // a second call changes nothing
    if (argc >= 2 && std::strcmp(argv[1], ServerCrash::kSelfTestSwitch) == 0)
        return ServerCrash::RunSelfTest(argc >= 3 ? argv[2] : "", &FaultHere);
    return 0;
}
