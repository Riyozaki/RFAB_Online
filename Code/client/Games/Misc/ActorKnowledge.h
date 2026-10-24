#pragma once

struct ActorKnowledgeDetectionState : NiRefObject
{
    ~ActorKnowledgeDetectionState() override;

    int32_t level;    // +0x10 (-1000 = undetected)
    uint8_t inLos;    // +0x14
    uint8_t hasLos;   // +0x15
    uint8_t unk16;    // +0x16
    uint8_t pad17;    // +0x17
};

struct ActorKnowledge : NiRefObject
{
    int32_t hOwner;                                       // +0x10
    int32_t hTarget;                                      // +0x14
    int32_t eFactionFightReaction;                        // +0x18
    uint32_t shouldAttackTargetTimeStamp;                 // +0x1C
    NiPointer<ActorKnowledgeDetectionState> spDetection;  // +0x20
};
static_assert(offsetof(ActorKnowledge, spDetection) == 0x20);

