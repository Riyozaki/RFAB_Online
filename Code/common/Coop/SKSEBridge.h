#pragma once
#include <cstddef>
#include <cstdint>

// Minimal public SKSE64 2.0.20 ABI, pinned at ianpatt/skse64 0d6c91dc84f1d384da113bec9e298867a5c86ce1,
// skse64/PluginAPI.h. No game-layout guesses, CommonLib dependency or engine pointers cross this boundary.
namespace Coop::SKSE
{
using PluginHandle = uint32_t;
struct PluginInfo { uint32_t InfoVersion; const char* Name; uint32_t Version; };
struct Interface
{
    uint32_t Version, Runtime, EditorVersion, IsEditor;
    void* (*QueryInterface)(uint32_t);
    PluginHandle (*GetPluginHandle)();
    uint32_t (*GetReleaseIndex)();
    const PluginInfo* (*GetPluginInfo)(const char*);
};
struct Serialization
{
    uint32_t Version;
    using Callback = void (*)(Serialization*);
    void (*SetUniqueID)(PluginHandle, uint32_t);
    void (*SetRevertCallback)(PluginHandle, Callback);
    void (*SetSaveCallback)(PluginHandle, Callback);
    void (*SetLoadCallback)(PluginHandle, Callback);
    void (*SetFormDeleteCallback)(PluginHandle, void (*)(uint64_t));
    bool (*WriteRecord)(uint32_t, uint32_t, const void*, uint32_t);
    bool (*OpenRecord)(uint32_t, uint32_t);
    bool (*WriteRecordData)(const void*, uint32_t);
    bool (*GetNextRecordInfo)(uint32_t*, uint32_t*, uint32_t*);
    uint32_t (*ReadRecordData)(void*, uint32_t);
    bool (*ResolveHandle)(uint64_t, uint64_t*);
    bool (*ResolveFormId)(uint32_t, uint32_t*);
};
struct Messaging
{
    struct Message { const char* Sender; uint32_t Type, Length; void* Data; };
    uint32_t Version;
    bool (*RegisterListener)(PluginHandle, const char*, void (*)(Message*));
    bool (*Dispatch)(PluginHandle, uint32_t, void*, uint32_t, const char*);
    void* (*GetEventDispatcher)(uint32_t);
};
inline constexpr uint32_t kBridgeVersion = 1, kRecord = 0x53434652; // RFCS, little endian
inline constexpr uint32_t kRuntime1597 = 0x01050610; // MAKE_EXE_VERSION(1,5,97): minor 8 bits, build 12 bits
// Correct packed runtime is checked explicitly by the adapter; this table is versioned independently.
struct Host
{
    uint32_t Version, Size;
    void (*Registered)();
    void (*Save)(Serialization*);
    void (*Load)(Serialization*);
    void (*Revert)(Serialization*);
    void (*Message)(uint32_t, bool);
};
static_assert(offsetof(Serialization, SetUniqueID) == 8);
static_assert(offsetof(Serialization, ResolveFormId) == 96);
static_assert(sizeof(Host) == 48);
} // namespace Coop::SKSE
