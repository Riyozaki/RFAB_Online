#pragma once

#include "TESForm.h"

struct BGSListForm : TESForm
{
    void AddForm(TESForm* apForm) noexcept;

    [[nodiscard]] bool ContainsFormId(uint32_t aFormId) const noexcept
    {
        if (aFormId == 0)
            return false;

        for (uint32_t i = 0; i < ArrayOfForms.length; ++i)
        {
            if (ArrayOfForms[i] && ArrayOfForms[i]->formID == aFormId)
                return true;
        }

        if (pScriptAddedTempFormA)
        {
            for (uint32_t i = 0; i < pScriptAddedTempFormA->length; ++i)
            {
                if ((*pScriptAddedTempFormA)[i] == aFormId)
                    return true;
            }
        }

        return false;
    }

    template <class TFunc> void ForEachFormId(TFunc&& aFunc) const
    {
        for (uint32_t i = 0; i < ArrayOfForms.length; ++i)
        {
            if (ArrayOfForms[i] && ArrayOfForms[i]->formID != 0)
                aFunc(ArrayOfForms[i]->formID);
        }

        if (pScriptAddedTempFormA)
        {
            for (uint32_t i = 0; i < pScriptAddedTempFormA->length; ++i)
            {
                const uint32_t formId = (*pScriptAddedTempFormA)[i];
                if (formId != 0)
                    aFunc(formId);
            }
        }
    }

    GameArray<TESForm*> ArrayOfForms{};
    GameArray<uint32_t>* pScriptAddedTempFormA{};
    uint32_t iScriptAddedFormCount{};
};

static_assert(sizeof(BGSListForm) == 0x48);
