#pragma once

#include <type_traits>
#include <utility>

#include "BSCore/BSIntrusiveRefCounted.h"

template <class T> struct BSTSmartPointer
{
    BSTSmartPointer() noexcept = default;
    BSTSmartPointer(std::nullptr_t) noexcept {}

    explicit BSTSmartPointer(T* apPointer) noexcept
        : m_pPointer(apPointer)
    {
        if (m_pPointer)
            m_pPointer->IncRef();
    }

    BSTSmartPointer(const BSTSmartPointer& acRhs) noexcept
        : m_pPointer(acRhs.m_pPointer)
    {
        if (m_pPointer)
            m_pPointer->IncRef();
    }

    BSTSmartPointer(BSTSmartPointer&& aRhs) noexcept
        : m_pPointer(std::exchange(aRhs.m_pPointer, nullptr))
    {
    }

    ~BSTSmartPointer()
    {
        static_assert(std::has_virtual_destructor_v<T>);

        if (m_pPointer && m_pPointer->DecRef() == 0)
            delete m_pPointer;
    }

    BSTSmartPointer& operator=(BSTSmartPointer aRhs) noexcept
    {
        std::swap(m_pPointer, aRhs.m_pPointer);
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return m_pPointer; }
    [[nodiscard]] T* operator->() const noexcept { return m_pPointer; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_pPointer != nullptr; }

private:
    T* m_pPointer{nullptr};
};

static_assert(sizeof(BSTSmartPointer<BSIntrusiveRefCounted>) == sizeof(void*));
