#pragma once

#include <Games/Memory.h>
#include <iterator>

template <class T> struct GameArray
{
    T* data;
    alignas(sizeof(void*)) uint32_t capacity; // Might be size_t
    alignas(sizeof(void*)) uint32_t length;   // Might be size_t

    GameArray()
        : data{nullptr}
        , capacity{}
        , length{0}
    {
    }
    ~GameArray() = default;

    T& operator[](uint32_t aIndex) { return data[aIndex]; }
    const T& operator[](uint32_t aIndex) const { return data[aIndex]; }

    void Resize(size_t aLength) noexcept;

    // Range for loop & C++20 <algorithm> compatibility
    struct Iterator
    {
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = T*;
        using reference = T&;

        Iterator() = default;
        Iterator(T* apEntry)
            : m_pEntry(apEntry)
        {
        }
        Iterator& operator++()
        {
            ++m_pEntry;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }
        bool operator==(const Iterator& acRhs) const { return m_pEntry == acRhs.m_pEntry; }
        bool operator!=(const Iterator& acRhs) const { return m_pEntry != acRhs.m_pEntry; }
        T& operator*() const { return *m_pEntry; }

    private:
        T* m_pEntry{nullptr};
    };

    Iterator begin() { return Iterator(&data[0]); }

    Iterator end() { return Iterator(&data[length]); }

    struct ConstIterator
    {
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = const T*;
        using reference = const T&;

        ConstIterator() = default;
        ConstIterator(const T* apEntry)
            : m_pEntry(apEntry)
        {}
        ConstIterator& operator++()
        {
            ++m_pEntry;
            return *this;
        }
        ConstIterator operator++(int)
        {
            ConstIterator tmp = *this;
            ++(*this);
            return tmp;
        }
        bool operator==(const ConstIterator& acRhs) const { return m_pEntry == acRhs.m_pEntry; }
        bool operator!=(const ConstIterator& acRhs) const { return m_pEntry != acRhs.m_pEntry; }
        const T& operator*() const { return *m_pEntry; }
        
    private:
        const T* m_pEntry{nullptr};
    };

    ConstIterator begin() const { return ConstIterator(&data[0]); }

    ConstIterator end() const { return ConstIterator(&data[length]); }

    inline bool Empty() const noexcept { return length == 0; }
};

template <class T> void GameArray<T>::Resize(size_t aLength) noexcept
{
    if (capacity >= aLength)
    {
        length = aLength & 0xFFFFFFFF;
    }
    else
    {
        T* pNewData = reinterpret_cast<T*>(Memory::Allocate(sizeof(T) * aLength));
        std::copy(data, data + length, pNewData);
        Memory::Free(data);
        data = pNewData;
        capacity = length = aLength;
    }
}

static_assert(offsetof(GameArray<int>, length) == 0x10);
static_assert(sizeof(GameArray<int>) == 0x18);

template <class T> struct BSTSmallArray
{
    int32_t capacity; // 0
    T** data;         // 8
    uint32_t size;    // 10

    T* Get(uint32_t aIndex)
    {
        auto pData = reinterpret_cast<T**>(&data);
        if (capacity >= 0)
            pData = data;

        return pData[aIndex];
    }
};

#if TP_PLATFORM_64
static_assert(offsetof(BSTSmallArray<int>, size) == 0x10);
static_assert(sizeof(BSTSmallArray<int>) == 0x18);
#else
static_assert(offsetof(BSTSmallArray<int>, size) == 0x8);
static_assert(sizeof(BSTSmallArray<int>) == 0xC);
#endif

template <class T> struct GameList
{
    struct Entry
    {
        T* data;
        Entry* next;
    };

    Entry entry;

    inline bool Empty() const noexcept { return entry.data == nullptr; }
    inline size_t Size() const noexcept
    {
        if (entry.data == nullptr)
            return 0ULL;

        size_t size = 0;
        for (const Entry* current = &entry; current; current = current->next)
            size++;

        return size;
    }

    // Range for loop compatibility
    struct Iterator
    {
        Iterator(Entry* apEntry)
            : m_pEntry(apEntry)
        {
        }
        Iterator operator++()
        {
            m_pEntry = m_pEntry->next;
            return *this;
        }
        bool operator!=(const Iterator& acRhs) const { return m_pEntry != acRhs.m_pEntry; }
        T* operator*() const { return m_pEntry->data; }

    private:
        Entry* m_pEntry;
    };

    Iterator begin() { return Iterator(&entry); }

    Iterator end() { return Iterator(nullptr); }
};

// NOTE: `GameValueList` is a corrected version of `GameList` that matches the game's actual memory layout.
// GameList stores T* (pointer to value) in each entry, but the game embeds
// T directly (see CommonLibSSE's `BSSimpleList`, which is actually implemented correctly).
// This mismatch causes silent data misinterpretation: iterators return T* when the bytes are actually T.
// TODO: Migrate from GameList to this (and rename)
template <class T> struct GameValueList
{
    struct Entry
    {
        T data;
        Entry* next;
    };

    Entry entry{};

    inline bool Empty() const noexcept
    {
        return !entry.next && !static_cast<bool>(entry.data);
    }

    inline size_t Size() const noexcept
    {
        if (Empty())
            return 0;

        size_t size = 0;
        for (const Entry* current = &entry; current; current = current->next)
            size++;

        return size;
    }

    // Range for loop compatibility
    struct Iterator
    {
        Iterator(Entry* apEntry) : m_pEntry(apEntry)
        {
        }
        Iterator& operator++()
        {
            m_pEntry = m_pEntry->next;
            return *this;
        }
        bool operator!=(const Iterator& acRhs) const
        {
            return m_pEntry != acRhs.m_pEntry;
        }
        T& operator*() const
        {
            return m_pEntry->data;
        }

      private:
        Entry* m_pEntry;
    };

    Iterator begin()
    {
        return Empty() ? end() : Iterator(&entry);
    }

    Iterator end()
    {
        return Iterator(nullptr);
    }
};

#if TP_PLATFORM_64
static_assert(offsetof(GameArray<int>, length) == 0x10);
static_assert(sizeof(GameArray<int>) == 0x18);
#else
static_assert(offsetof(GameArray<int>, length) == 0x8);
#endif

struct NiRefObject
{
    virtual ~NiRefObject();

    virtual void Destroy();

    void IncRef();
    void DecRef();
    bool Release();

    volatile LONG refCount;
    uint32_t padC;
};

#if TP_PLATFORM_64
static_assert(sizeof(NiRefObject) == 0x10);
#endif

struct BSHandleRefObject : NiRefObject
{
    void DecRefHandle()
    {
        if ((InterlockedDecrement(&refCount) & 0x3FF) == 0)
            Destroy();
    }
};

struct NiPoint3 : glm::vec3
{
    NiPoint3()
        : glm::vec3()
    {
    }
    NiPoint3(const glm::vec3& acRhs)
        : glm::vec3(acRhs)
    {
    }

    float* AsArray() { return &x; }
};

static_assert(sizeof(NiPoint3) == 12);
static_assert(offsetof(NiPoint3, x) == 0);
static_assert(offsetof(NiPoint3, y) == 4);
static_assert(offsetof(NiPoint3, z) == 8);

struct NiPoint2 : glm::vec2
{
    float* AsArray() { return &x; }
};

static_assert(sizeof(NiPoint2) == 8);
static_assert(offsetof(NiPoint2, x) == 0);
static_assert(offsetof(NiPoint2, y) == 4);

struct BSRecursiveLock
{
    volatile DWORD m_tid;
    volatile LONG m_counter;

    void Lock();
    void Unlock();
};

template <class T> struct BSScopedLock
{
    BSScopedLock(T& aLock)
        : m_lock(aLock)
    {
        m_lock.Lock();
    }
    ~BSScopedLock() { m_lock.Unlock(); }

private:
    T& m_lock;
};

template <class T> struct GamePtr
{
    GamePtr() noexcept
        : m_pPointer(nullptr)
    {
    }

    GamePtr(std::nullptr_t) noexcept
        : m_pPointer(nullptr)
    {
    }

    GamePtr(T* apPointer) noexcept
        : m_pPointer(apPointer)
    {
        if (m_pPointer)
            m_pPointer->handleRefObject.IncRef();
    }

    GamePtr(const GamePtr<T>& acRhs) noexcept
        : m_pPointer(acRhs.m_pPointer)
    {
        if (m_pPointer)
            m_pPointer->handleRefObject.IncRef();
    }

    GamePtr(GamePtr<T>&& aRhs) noexcept
        : m_pPointer(aRhs.m_pPointer)
    {
        aRhs.m_pPointer = nullptr;
    }

    GamePtr& operator=(const GamePtr<T>& acRhs) noexcept
    {
        if (this != &acRhs && m_pPointer != acRhs.m_pPointer)
        {
            T* const pNew = acRhs.m_pPointer;
            if (pNew)
                pNew->handleRefObject.IncRef();
            Release();
            m_pPointer = pNew;
        }

        return *this;
    }

    GamePtr& operator=(T* apPointer) noexcept
    {
        if (m_pPointer != apPointer)
        {
            if (apPointer)
                apPointer->handleRefObject.IncRef();
            Release();
            m_pPointer = apPointer;
        }

        return *this;
    }

    GamePtr& operator=(std::nullptr_t) noexcept
    {
        Release();
        return *this;
    }

    GamePtr& operator=(GamePtr<T>&& aRhs) noexcept
    {
        if (this != &aRhs)
        {
            Release();
            m_pPointer = aRhs.m_pPointer;
            aRhs.m_pPointer = nullptr;
        }

        return *this;
    }

    T* Get() const noexcept { return m_pPointer; }
    T* operator->() const noexcept { return m_pPointer; }
    T* operator*() const noexcept { return m_pPointer; }
    operator T*() const noexcept { return m_pPointer; }
    explicit operator bool() const noexcept { return m_pPointer != nullptr; }

    ~GamePtr() noexcept { Release(); }

    void Release() noexcept
    {
        T* const pOld = m_pPointer;
        m_pPointer = nullptr;
        if (pOld)
            pOld->handleRefObject.DecRefHandle();
    }

private:
    T* m_pPointer{nullptr};
};

enum ETiltedFlags : uint8_t
{
    kRemote = 1 << 0
};

template <class Target, class Source> Target* niptr_cast(const Source& acSrc)
{
    return static_cast<Target*>(acSrc.object);
}

template <uint32_t FREE_LIST_BITS = 21, uint32_t AGE_SHIFT = 5> struct BSUntypedPointerHandle
{
    uint32_t iBits = 0;
};

template <class T, class THandle = BSUntypedPointerHandle<>> struct BSPointerHandle
{
    BSPointerHandle() = default;

    operator bool() const { return handle.iBits != 0; }

    THandle handle{};
};

struct BSReadWriteLock
{
    uint32_t uiWriterThread;
    volatile uint32_t uiLock;
};
