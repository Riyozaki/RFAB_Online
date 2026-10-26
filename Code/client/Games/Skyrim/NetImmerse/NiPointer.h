#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

template <class T> struct NiPointer
{
    constexpr NiPointer() noexcept
        : object(nullptr)
    {
    }

    constexpr NiPointer(std::nullptr_t) noexcept
        : object(nullptr)
    {
    }

    NiPointer(T* apObj) noexcept
        : object(apObj)
    {
        SafeAddRef<T>(object);
    }

    NiPointer(const NiPointer& acRhs) noexcept
        : object(acRhs.object)
    {
        SafeAddRef<T>(object);
    }

    NiPointer(NiPointer&& aRhs) noexcept
        : object(aRhs.object)
    {
        aRhs.object = nullptr;
    }

    ~NiPointer() noexcept
    {
        Reset();
    }

    NiPointer<T>& operator=(const NiPointer& acRhs) noexcept
    {
        if (this != &acRhs && object != acRhs.object)
        {
            T* const pNew = acRhs.object;
            SafeAddRef<T>(pNew);
            T* const pOld = object;
            object = pNew;
            SafeRelease<T>(pOld);
        }

        return *this;
    }

    NiPointer<T>& operator=(NiPointer&& aRhs) noexcept
    {
        if (this != &aRhs)
        {
            T* const pOld = object;
            object = aRhs.object;
            aRhs.object = nullptr;
            SafeRelease<T>(pOld);
        }

        return *this;
    }

    NiPointer<T>& operator=(T* aRhs) noexcept
    {
        if (object != aRhs)
        {
            SafeAddRef<T>(aRhs);
            T* const pOld = object;
            object = aRhs;
            SafeRelease<T>(pOld);
        }

        return *this;
    }

    NiPointer<T>& operator=(std::nullptr_t) noexcept
    {
        Reset();
        return *this;
    }

    void Reset() noexcept
    {
        T* const pOld = object;
        object = nullptr;
        SafeRelease<T>(pOld);
    }

    [[nodiscard]] constexpr T* Get() const noexcept { return object; }
    [[nodiscard]] constexpr T* operator->() const noexcept { return object; }
    [[nodiscard]] constexpr std::add_lvalue_reference_t<T> operator*() const noexcept { return *object; }

    [[nodiscard]] explicit constexpr operator bool() const noexcept
    {
        return object != nullptr;
    }

    T* object{nullptr};

private:
    template <class U>
    static void SafeAddRef(U* apObj) noexcept
    {
        if (!apObj)
            return;

        if constexpr (requires(U* p) { p->handleRefObject.IncRef(); })
        {
            apObj->handleRefObject.IncRef();
        }
        else if constexpr (requires(U* p) { p->IncRef(); })
        {
            apObj->IncRef();
        }
    }

    template <class U>
    static void SafeRelease(U* apObj) noexcept
    {
        if (!apObj)
            return;

        if constexpr (requires(U* p) { p->handleRefObject.DecRefHandle(); })
        {
            apObj->handleRefObject.DecRefHandle();
        }
        else if constexpr (requires(U* p) { p->DecRef(); })
        {
            apObj->DecRef();
        }
    }
};

static_assert(sizeof(NiPointer<void>) == sizeof(void*), "NiPointer<T> must remain pointer-sized for Skyrim SE ABI");
