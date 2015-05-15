/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#ifndef UAVCAN_UTIL_MULTISET_HPP_INCLUDED
#define UAVCAN_UTIL_MULTISET_HPP_INCLUDED

#include <cassert>
#include <cstdlib>
#include <uavcan/util/linked_list.hpp>
#include <uavcan/build_config.hpp>
#include <uavcan/dynamic_memory.hpp>
#include <uavcan/util/templates.hpp>
#include <uavcan/util/placement_new.hpp>

#if !defined(UAVCAN_CPP_VERSION) || !defined(UAVCAN_CPP11)
# error UAVCAN_CPP_VERSION
#endif

namespace uavcan
{
/**
 * Slow but memory efficient unordered multiset. Unlike Map<>, this container does not move objects, so
 * they don't have to be copyable.
 *
 * Items can be allocated in a static buffer or in the node's memory pool if the static buffer is exhausted.
 */
template <typename T>
class UAVCAN_EXPORT MultisetBase : Noncopyable
{
protected:
    struct Item : ::uavcan::Noncopyable
    {
        T* ptr;

#if UAVCAN_CPP_VERSION >= UAVCAN_CPP11
        alignas(T) unsigned char pool[sizeof(T)];       ///< Memory efficient version
#else
        union
        {
            unsigned char pool[sizeof(T)];
            long double _aligner1_;
            long long _aligner2_;
        };
#endif

        Item()
            : ptr(NULL)
        {
            fill_n(pool, sizeof(pool), static_cast<unsigned char>(0));
        }

        ~Item() { destroy(); }

        bool isConstructed() const { return ptr != NULL; }

        void destroy()
        {
            if (ptr != NULL)
            {
                ptr->~T();
                ptr = NULL;
                fill_n(pool, sizeof(pool), static_cast<unsigned char>(0));
            }
        }
    };

private:
    struct Chunk : LinkedListNode<Chunk>, ::uavcan::Noncopyable
    {
        enum { NumItems = (MemPoolBlockSize - sizeof(LinkedListNode<Chunk>)) / sizeof(Item) };
        Item items[NumItems];

        Chunk()
        {
            StaticAssert<(static_cast<unsigned>(NumItems) > 0)>::check();
            IsDynamicallyAllocatable<Chunk>::check();
            UAVCAN_ASSERT(!items[0].isConstructed());
        }

        static Chunk* instantiate(IPoolAllocator& allocator)
        {
            void* const praw = allocator.allocate(sizeof(Chunk));
            if (praw == NULL)
            {
                return NULL;
            }
            return new (praw) Chunk();
        }

        static void destroy(Chunk*& obj, IPoolAllocator& allocator)
        {
            if (obj != NULL)
            {
                obj->~Chunk();
                allocator.deallocate(obj);
                obj = NULL;
            }
        }

        Item* findFreeSlot()
        {
            for (unsigned i = 0; i < static_cast<unsigned>(NumItems); i++)
            {
                if (!items[i].isConstructed())
                {
                    return items + i;
                }
            }
            return NULL;
        }
    };

    LinkedListRoot<Chunk> list_;
    IPoolAllocator& allocator_;
#if !UAVCAN_TINY
    Item* const static_;
    const unsigned num_static_entries_;
#endif

    Item* findOrCreateFreeSlot();

    void compact();

    struct YesPredicate
    {
        bool operator()(const T&) const { return true; }
    };

    struct IndexPredicate : ::uavcan::Noncopyable
    {
        unsigned index;
        IndexPredicate(unsigned target_index) : index(target_index) { }
        bool operator()(const T&) { return index--==0; }
    };

    struct ComparingPredicate
    {
        const T& reference;
        ComparingPredicate(const T& ref) : reference(ref) { }
        bool operator()(const T& sample) { return reference == sample; }
    };

protected:
#if UAVCAN_TINY
    MultisetBase(IPoolAllocator& allocator)
        : allocator_(allocator)
    {
        UAVCAN_ASSERT(Item() == Item());
    }
#else
    MultisetBase(Item* static_buf, unsigned num_static_entries, IPoolAllocator& allocator)
        : allocator_(allocator)
        , static_(static_buf)
        , num_static_entries_(num_static_entries)
    { }
#endif

    /// Derived class destructor must call removeAll();
    ~MultisetBase()
    {
        UAVCAN_ASSERT(getSize() == 0);
    }

public:
    /**
     * This is needed for testing.
     */
    enum { NumItemsPerDynamicChunk = Chunk::NumItems };

    /**
     * Adds one item and returns a pointer to it.
     * If add fails due to lack of memory, NULL will be returned.
     */
    T* add()
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T();
        return item->ptr;
    }

    template <typename P1>
    T* add(P1 p1)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1);
        return item->ptr;
    }

    template <typename P1, typename P2>
    T* add(P1 p1, P2 p2)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1, p2);
        return item->ptr;
    }

    template <typename P1, typename P2, typename P3>
    T* add(P1 p1, P2 p2, P3 p3)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1, p2, p3);
        return item->ptr;
    }

    /**
     * @ref removeMatching()
     */
    enum RemoveStrategy { RemoveOne, RemoveAll };

    /**
     * Removes entries where the predicate returns true.
     * Predicate prototype:
     *  bool (T& item)
     */
    template <typename Predicate>
    void removeMatching(Predicate predicate, RemoveStrategy strategy);

    template <typename Predicate>
    void removeAllMatching(Predicate predicate) { removeMatching<Predicate>(predicate, RemoveAll); }

    template <typename Predicate>
    void removeFirstMatching(Predicate predicate) { removeMatching<Predicate>(predicate, RemoveOne); }

    void removeFirst(const T& ref) { removeFirstMatching(ComparingPredicate(ref)); }

    /**
     * Returns first entry where the predicate returns true.
     * Predicate prototype:
     *  bool (const T& item)
     */
    template <typename Predicate>
    T* find(Predicate predicate);

    template <typename Predicate>
    const T* find(Predicate predicate) const
    {
        return const_cast<MultisetBase<T>*>(this)->find<Predicate>(predicate);
    }

    /**
     * Removes all items; all pool memory will be released.
     */
    void removeAll() { removeAllMatching(YesPredicate()); }

    /**
     * Returns an item located at the specified position from the beginning.
     * Note that any insertion or deletion may greatly disturb internal ordering, so use with care.
     * If index is greater than or equal the number of items, null pointer will be returned.
     */
    T* getByIndex(unsigned index)
    {
        IndexPredicate predicate(index);
        return find<IndexPredicate&>(predicate);
    }

    const T* getByIndex(unsigned index) const
    {
        return const_cast<MultisetBase<T>*>(this)->getByIndex(index);
    }

    /**
     * This is O(1)
     */
    bool isEmpty() const { return find(YesPredicate()) == NULL; }

    /**
     * Counts number of items stored.
     * Best case complexity is O(N).
     */
    unsigned getSize() const { return getNumStaticItems() + getNumDynamicItems(); }

    /**
     * For testing, do not use directly.
     */
    unsigned getNumStaticItems() const;
    unsigned getNumDynamicItems() const;
};


template <typename T, unsigned NumStaticEntries = 0>
class UAVCAN_EXPORT Multiset : public MultisetBase<T>
{
    typename MultisetBase<T>::Item static_[NumStaticEntries];

public:

#if !UAVCAN_TINY

    // This instantiation will not be valid in UAVCAN_TINY mode
    explicit Multiset(IPoolAllocator& allocator)
        : MultisetBase<T>(static_, NumStaticEntries, allocator)
    { }

    ~Multiset() { this->removeAll(); }

#endif // !UAVCAN_TINY
};


template <typename T>
class UAVCAN_EXPORT Multiset<T, 0> : public MultisetBase<T>
{
public:
    explicit Multiset(IPoolAllocator& allocator)
#if UAVCAN_TINY
        : MultisetBase<T>(allocator)
#else
        : MultisetBase<T>(NULL, 0, allocator)
#endif
    { }

    ~Multiset() { this->removeAll(); }
};

// ----------------------------------------------------------------------------

/*
 * MultisetBase<>
 */
template <typename T>
typename MultisetBase<T>::Item* MultisetBase<T>::findOrCreateFreeSlot()
{
#if !UAVCAN_TINY
    // Search in static pool
    for (unsigned i = 0; i < num_static_entries_; i++)
    {
        if (!static_[i].isConstructed())
        {
            return &static_[i];
        }
    }
#endif

    // Search in dynamic pool
    {
        Chunk* p = list_.get();
        while (p)
        {
            Item* const dyn = p->findFreeSlot();
            if (dyn != NULL)
            {
                return dyn;
            }
            p = p->getNextListNode();
        }
    }

    // Create new dynamic chunk
    Chunk* const chunk = Chunk::instantiate(allocator_);
    if (chunk == NULL)
    {
        return NULL;
    }
    list_.insert(chunk);
    return &chunk->items[0];
}

template <typename T>
void MultisetBase<T>::compact()
{
    Chunk* p = list_.get();
    while (p)
    {
        Chunk* const next = p->getNextListNode();
        bool remove_this = true;
        for (int i = 0; i < Chunk::NumItems; i++)
        {
            if (p->items[i].isConstructed())
            {
                remove_this = false;
                break;
            }
        }
        if (remove_this)
        {
            list_.remove(p);
            Chunk::destroy(p, allocator_);
        }
        p = next;
    }
}

template <typename T>
template <typename Predicate>
void MultisetBase<T>::removeMatching(Predicate predicate, const RemoveStrategy strategy)
{
    unsigned num_removed = 0;

#if !UAVCAN_TINY
    for (unsigned i = 0; i < num_static_entries_; i++)
    {
        if (static_[i].isConstructed())
        {
            if (predicate(*static_[i].ptr))
            {
                num_removed++;
                static_[i].destroy();
            }
        }

        if ((num_removed > 0) && (strategy == RemoveOne))
        {
            break;
        }
    }
#endif

    Chunk* p = list_.get();
    while (p)
    {
        if ((num_removed > 0) && (strategy == RemoveOne))
        {
            break;
        }

        for (int i = 0; i < Chunk::NumItems; i++)
        {
            Item& item = p->items[i];
            if (item.isConstructed())
            {
                if (predicate(*item.ptr))
                {
                    num_removed++;
                    item.destroy();
                }
            }
        }

        p = p->getNextListNode();
    }

    if (num_removed > 0)
    {
        compact();
    }
}

template <typename T>
template <typename Predicate>
T* MultisetBase<T>::find(Predicate predicate)
{
#if !UAVCAN_TINY
    for (unsigned i = 0; i < num_static_entries_; i++)
    {
        if (static_[i].isConstructed())
        {
            if (predicate(*static_[i].ptr))
            {
                return static_[i].ptr;
            }
        }
    }
#endif

    Chunk* p = list_.get();
    while (p)
    {
        for (int i = 0; i < Chunk::NumItems; i++)
        {
            if (p->items[i].isConstructed())
            {
                if (predicate(*p->items[i].ptr))
                {
                    return p->items[i].ptr;
                }
            }
        }
        p = p->getNextListNode();
    }
    return NULL;
}

template <typename T>
unsigned MultisetBase<T>::getNumStaticItems() const
{
    unsigned num = 0;
#if !UAVCAN_TINY
    for (unsigned i = 0; i < num_static_entries_; i++)
    {
        num += static_[i].isConstructed() ? 1U : 0U;
    }
#endif
    return num;
}

template <typename T>
unsigned MultisetBase<T>::getNumDynamicItems() const
{
    unsigned num = 0;
    Chunk* p = list_.get();
    while (p)
    {
        for (int i = 0; i < Chunk::NumItems; i++)
        {
            num += p->items[i].isConstructed() ? 1U : 0U;
        }
        p = p->getNextListNode();
    }
    return num;
}

}

#endif // Include guard