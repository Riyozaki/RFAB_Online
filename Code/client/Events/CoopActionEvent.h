#pragma once
#include <Structs/CoopProtocol.h>

// Main-thread only, after CoopService has checked session, generation and ordered operation.
struct CoopActionEvent
{
    Coop::Packet Data;
};
