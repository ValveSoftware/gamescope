#pragma once

#include <cstdint>
#include <atomic>

namespace gamescope
{
    class RcObject
    {
    public:
        virtual ~RcObject()
        {
        }

        uint32_t IncRef()
        {
            uint32_t uRefCount = m_uRefCount++;
            if ( !uRefCount )
                IncRefPrivate();
            return uRefCount;
        }

        uint32_t DecRef()
        {
            uint32_t uRefCount = --m_uRefCount;
            if ( !uRefCount )
                DecRefPrivate();
            return uRefCount;
        }

        uint32_t IncRefPrivate()
        {
            return m_uRefPrivate++;
        }

        uint32_t DecRefPrivate()
        {
            uint32_t uRefPrivate = --m_uRefPrivate;
            if ( !uRefPrivate )
            {
                m_uRefPrivate += 0x80000000;
                delete this;
            }
            
            return uRefPrivate;
        }

        uint32_t GetRefCount() const
        {
            return m_uRefCount;
        }

        uint32_t GetRefCountPrivate() const
        {
            return m_uRefPrivate;
        }

        bool HasLiveReferences() const
        {
            return bool( m_uRefCount.load() | ( m_uRefPrivate.load() & 0x7FFFFFFF ) );
        }

    private:
        std::atomic<uint32_t> m_uRefCount{ 0u };
        std::atomic<uint32_t> m_uRefPrivate{ 0u };
    };

    class IRcObject : public RcObject
    {
    public:
        virtual uint32_t IncRef()
        {
            return RcObject::IncRef();
        }

        virtual uint32_t DecRef()
        {
            return RcObject::DecRef();
        }
    };

    template <typename T, bool Public>
    struct RcRef_
    {
        static void IncRef( T* pObject ) { pObject->IncRef(); }
        static void DecRef( T* pObject ) { pObject->DecRef(); }
    };

    template <typename T>
    struct RcRef_<T, false>
    {
        static void IncRef( T* pObject ) { pObject->IncRefPrivate(); }
        static void DecRef( T* pObject ) { pObject->DecRefPrivate(); }
    };

    template <typename T, bool Public = true>
    class Rc
    {
        template <typename Tx, bool Publicx>
        friend class Rc;

        using RcRef = RcRef_<T, Public>;
    public:
        Rc() { }
        Rc( std::nullptr_t ) { }

        Rc( T* pObject )
            : m_pObject{ pObject }
        {
            this->IncRef();
        }

        Rc( const Rc& other )
            : m_pObject{ other.m_pObject }
        {
            this->IncRef();
        }

        template <typename Tx, bool Publicx>
        Rc( const Rc<Tx, Publicx>& other )
            : m_pObject{ other.m_pObject }
        {
            this->IncRef();
        }

        Rc( Rc&& other )
            : m_pObject{ other.m_pObject }
        {
            other.m_pObject = nullptr;
        }

        template <typename Tx>
        Rc( Rc<Tx, Public>&& other )
            : m_pObject{ other.m_pObject }
        {
            other.m_pObject = nullptr;
        }

        Rc& operator = ( std::nullptr_t )
        {
            this->DecRef();
            m_pObject = nullptr;
            return *this;
        }

        Rc& operator = ( const Rc& other )
        {
            other.IncRef();
            this->DecRef();
            m_pObject = other.m_pObject;
            return *this;
        }

        template <typename Tx>
        Rc& operator = ( const Rc<Tx, Public>& other )
        {
            other.IncRef();
            this->DecRef();
            m_pObject = other.m_pObject;
            return *this;
        }

        Rc& operator = ( Rc&& other )
        {
            this->DecRef();
            this->m_pObject = other.m_pObject;
            other.m_pObject = nullptr;
            return *this;
        }

        template <typename Tx>
        Rc& operator = ( Rc<Tx, Public>&& other )
        {
            this->DecRef();
            this->m_pObject = other.m_pObject;
            other.m_pObject = nullptr;
            return *this;
        }

        ~Rc()
        {
            this->DecRef();
        }

        T& operator *  () const { return *m_pObject; }
        T* operator -> () const { return  m_pObject; }
        T* get() const { return m_pObject; }

        bool operator == ( const Rc& other ) const { return m_pObject == other.m_pObject; }
        bool operator != ( const Rc& other ) const { return m_pObject != other.m_pObject; }

        bool operator == ( T *pOther ) const { return m_pObject == pOther; }
        bool operator != ( T *pOther ) const { return m_pObject == pOther; }

        bool operator == ( std::nullptr_t ) const { return m_pObject == nullptr; }
        bool operator != ( std::nullptr_t ) const { return m_pObject != nullptr; }

        operator bool() const { return m_pObject != nullptr; }

    private:
        T* m_pObject = nullptr;

        inline void IncRef() const
        {
            if ( m_pObject != nullptr )
                RcRef::IncRef( m_pObject );
        }

        inline void DecRef() const
        {
            if ( m_pObject != nullptr )
                RcRef::DecRef( m_pObject );
        }
    };

    template <typename T>
    using OwningRc = Rc<T, false>;
}