#include <Coop/SKSEBridge.h>
#include <windows.h>

namespace
{
using namespace Coop::SKSE;
const Host* s_host{};
void Message(Messaging::Message* message)
{
    if (!message || !s_host) return;
    // SKSE passes PostLoadGame success as a bool-sized POINTER VALUE (not bool*).
    s_host->Message(message->Type, message->Data != nullptr);
}
}
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const Coop::SKSE::Interface* skse, Coop::SKSE::PluginInfo* info)
{
    if (!info) return false;
    *info = {1, "RFAB_CoopState", 1};
    // SKSE64 MAKE_EXE_VERSION(1,5,97) = (1<<24)|(5<<16)|(97<<4).
    return skse && !skse->IsEditor && skse->Runtime == Coop::SKSE::kRuntime1597;
}
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const Coop::SKSE::Interface* skse)
{
    if (!skse || skse->IsEditor || skse->Runtime != Coop::SKSE::kRuntime1597) return false;
    using GetHost = const Coop::SKSE::Host* (*)(uint32_t);
    const auto get = reinterpret_cast<GetHost>(GetProcAddress(GetModuleHandleW(nullptr), "RFAB_GetCoopBridge"));
    if (!get) return false; // never installs into an ordinary single-player executable
    auto* serialization = static_cast<Coop::SKSE::Serialization*>(skse->QueryInterface(3));
    auto* messaging = static_cast<Coop::SKSE::Messaging*>(skse->QueryInterface(5));
    const auto* host = get(Coop::SKSE::kBridgeVersion);
    if (!host || host->Version != Coop::SKSE::kBridgeVersion || host->Size != sizeof(*host) ||
        !serialization || serialization->Version < 4 || !messaging || messaging->Version < 2) return false;
    s_host = host;
    const auto plugin = skse->GetPluginHandle();
    if (!messaging->RegisterListener(plugin, "SKSE", Message)) { s_host = nullptr; return false; }
    serialization->SetUniqueID(plugin, Coop::SKSE::kRecord);
    serialization->SetSaveCallback(plugin, host->Save);
    serialization->SetLoadCallback(plugin, host->Load);
    serialization->SetRevertCallback(plugin, host->Revert);
    host->Registered();
    return true;
}
