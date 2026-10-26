#pragma once

struct ActiveEffect;
struct BGSLoadFormBuffer;
struct bhkCharacterController;
struct TESObjectREFR;

struct InventoryEntry;

struct MiddleProcess
{
    // void SaveActiveEffects()
    void LoadActiveEffects(BGSLoadFormBuffer* apLoadGameBuffer);

    // 0xB0 - pitch
    uint8_t pad0[0xB8];
    float direction; // B8
    uint8_t padBC[0x1A0 - 0xBC];
    GameList<ActiveEffect>* ActiveEffects;
    uint8_t pad1A8[0x208 - 0x1A8];
    BSPointerHandle<TESObjectREFR> occupiedFurniture; // 0x208
    uint8_t pad20C[0x218 - 0x20C];
    BSPointerHandle<TESObjectREFR> commandingActor;   // 0x218
    uint8_t pad21C[0x220 - 0x21C];
    InventoryEntry* leftEquippedObject;
    uint8_t pad228[0x250 - 0x228];
    bhkCharacterController* charController;
    uint8_t pad258[0x260 - 0x258];
    InventoryEntry* rightEquippedObject;
    InventoryEntry* ammoEquippedObject; // could be more than just ammo
    // 0xB8 - direction
    //
    // 0x326 - bool lookat
};

static_assert(offsetof(MiddleProcess, direction) == 0xB8);
static_assert(offsetof(MiddleProcess, occupiedFurniture) == 0x208);
static_assert(offsetof(MiddleProcess, commandingActor) == 0x218);
static_assert(offsetof(MiddleProcess, leftEquippedObject) == 0x220);
static_assert(offsetof(MiddleProcess, charController) == 0x250);
static_assert(offsetof(MiddleProcess, rightEquippedObject) == 0x260);
