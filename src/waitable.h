#pragma once

#include <thread>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <functional>
#include <mutex>

#include "log.hpp"

extern LogScope g_WaitableLog;

timespec nanos_to_timespec( uint64_t ulNanos );

namespace gamescope
{
    class IWaitable
    {
    public:
        virtual ~IWaitable() {}

        virtual int GetFD() { return -1; }

        virtual void OnPollIn() {}
        virtual void OnPollOut() {}
        virtual void OnPollHangUp()
        {
            g_WaitableLog.errorf( "IWaitable hung up. Aborting." );
            abort();
        }

        void HandleEvents( uint32_t nEvents )
        {
            if ( nEvents & EPOLLIN )
                this->OnPollIn();
            if ( nEvents & EPOLLOUT )
                this->OnPollOut();
            if ( nEvents & EPOLLHUP )
                this->OnPollHangUp();
        }

        static void Drain( int nFD )
        {
            if ( nFD < 0 )
                return;

            char buf[1024];
            for (;;)
            {
                if ( read( nFD, buf, sizeof( buf ) ) < 0 )
                {
                    if ( errno != EAGAIN )
                        g_WaitableLog.errorf_errno( "Failed to drain IWaitable" );
                    break;
                }
            }
        }
    };

    class CNudgeWaitable final : public IWaitable
    {
    public:
        CNudgeWaitable()
        {
            if ( pipe2( m_nFDs, O_CLOEXEC | O_NONBLOCK ) != 0 )
                Shutdown();
        }

        ~CNudgeWaitable()
        {
            Shutdown();
        }

        void Shutdown()
        {
            for ( int i = 0; i < 2; i++ )
            {
                if ( m_nFDs[i] >= 0 )
                {
                    close( m_nFDs[i] );
                    m_nFDs[i] = -1;
                }
            }
        }

        void Drain()
        {
            IWaitable::Drain( m_nFDs[0] );
        }

        void OnPollIn() final
        {
            Drain();
        }

        bool Nudge()
        {
            return write( m_nFDs[1], "\n", 1 ) >= 0;
        }

        int GetFD() final { return m_nFDs[0]; }
    private:
        int m_nFDs[2] = { -1, -1 };
    };


    class CFunctionWaitable final : public IWaitable
    {
    public:
        CFunctionWaitable( int nFD, std::function<void()> fnPollFunc = nullptr )
            : m_nFD{ nFD }
            , m_fnPollFunc{ fnPollFunc }
        {
        }

        void OnPollIn() final
        {
            if ( m_fnPollFunc )
                m_fnPollFunc();
        }

        void Drain()
        {
            IWaitable::Drain( m_nFD );
        }

        int GetFD() final
        {
            return m_nFD;
        }
    private:
        int m_nFD;
        std::function<void()> m_fnPollFunc;
    };

    class ITimerWaitable : public IWaitable
    {
    public:
        ITimerWaitable()
        {
            m_nFD = timerfd_create( CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC );
			if ( m_nFD < 0 )
			{
				g_WaitableLog.errorf_errno( "Failed to create timerfd." );
				abort();
			}
        }

        ~ITimerWaitable()
        {
            Shutdown();
        }

        void Drain()
        {
            IWaitable::Drain( m_nFD );
        }

        void OnPollIn()
        {
            Drain();
        }

        void Shutdown()
        {
            if ( m_nFD >= 0 )
            {
                close( m_nFD );
                m_nFD = -1;
            }
        }

        void ArmTimer( uint64_t ulScheduledWakeupTime, bool bRepeatingRelative = false )
        {
            timespec wakeupTimeSpec = nanos_to_timespec( ulScheduledWakeupTime );

			itimerspec timerspec =
			{
				.it_interval = bRepeatingRelative ? wakeupTimeSpec : timespec{},
				.it_value = bRepeatingRelative ? timespec{} : wakeupTimeSpec,
			};
			if ( timerfd_settime( m_nFD, TFD_TIMER_ABSTIME, &timerspec, NULL ) < 0 )
				g_WaitableLog.errorf_errno( "timerfd_settime failed!" );
        }

        void DisarmTimer()
        {
            ArmTimer( 0ul, false );
        }

        int GetFD()
        {
            return m_nFD;
        }
    private:
        int m_nFD = -1;
    };

    class CTimerFunction final : public ITimerWaitable
    {
    public:
        CTimerFunction( std::function<void()> fnPollFunc )
            : m_fnPollFunc{ fnPollFunc }
        {
        }

        void OnPollIn() final
        {
            ITimerWaitable::OnPollIn();
            m_fnPollFunc();
        }
    private:
        std::function<void()> m_fnPollFunc;
    };

    template <size_t MaxEvents = 1024>
    class CWaiter
    {
    public:
        CWaiter()
            : m_nEpollFD{ epoll_create1( EPOLL_CLOEXEC ) }
        {
            AddWaitable( &m_NudgeWaitable );
        }

        ~CWaiter()
        {
            Shutdown();
        }

        void Shutdown()
        {
            if ( !m_bRunning )
                return;

            m_bRunning = false;
            Nudge();

            if ( m_nEpollFD >= 0 )
            {
                close( m_nEpollFD );
                m_nEpollFD = -1;
            }
        }

        bool AddWaitable( IWaitable *pWaitable, uint32_t nEvents = EPOLLIN | EPOLLHUP )
        {
            epoll_event event =
            {
                .events = nEvents,
                .data =
                {
                    .ptr = reinterpret_cast<void *>( pWaitable ),
                },
            };

            if ( epoll_ctl( m_nEpollFD, EPOLL_CTL_ADD, pWaitable->GetFD(), &event ) != 0 )
            {
                g_WaitableLog.errorf_errno( "Failed to add waitable" );
                return false;
            }

            return true;
        }

        void RemoveWaitable( IWaitable *pWaitable )
        {
            epoll_ctl( m_nEpollFD, EPOLL_CTL_DEL, pWaitable->GetFD(), nullptr );
        }

        int PollEvents( int nTimeOut = -1 )
        {
            epoll_event events[MaxEvents];

            for ( ;; )
            {
                int nEventCount = epoll_wait( m_nEpollFD, events, MaxEvents, nTimeOut );

                if ( !m_bRunning )
                    return 0;

                if ( nEventCount < 0 )
                {
                    if ( errno == EAGAIN || errno == EINTR )
                        continue;

                    g_WaitableLog.errorf_errno( "Failed to epoll_wait in CAsyncWaiter" );
                    return nEventCount;
                }

                for ( int i = 0; i < nEventCount; i++ )
                {
                    epoll_event &event = events[i];

                    IWaitable *pWaitable = reinterpret_cast<IWaitable *>( event.data.ptr );
                    pWaitable->HandleEvents( event.events );
                }

                return nEventCount;
            }
        }

        bool Nudge()
        {
            return m_NudgeWaitable.Nudge();
        }

        bool IsRunning()
        {
            return m_bRunning;
        }

    private:
        std::atomic<bool> m_bRunning = { true };
        CNudgeWaitable m_NudgeWaitable;

        int m_nEpollFD = -1;
    };

    // A raw pointer class that's compatible with shared/unique_ptr + Rc semantics
    // eg. .get(), etc.
    // for compatibility with structures that use other types that assume ownership/lifetime
    // in some way.
    template <typename T>
    class CRawPointer
    {
    public:
        CRawPointer() {}
        CRawPointer( std::nullptr_t ) {}

        CRawPointer( const CRawPointer &other )
            : m_pObject{ other.m_pObject }
        {
        }

        CRawPointer( CRawPointer&& other )
            : m_pObject{ other.m_pObject }
        {
            other.m_pObject = nullptr;
        }

        CRawPointer( T* pObject )
            : m_pObject{ pObject }
        {
        }

        CRawPointer& operator = ( std::nullptr_t )
        {
            m_pObject = nullptr;
            return *this;
        }

        CRawPointer& operator = ( const CRawPointer& other )
        {
            m_pObject = other.m_pObject;
            return *this;
        }

        CRawPointer& operator = ( CRawPointer&& other )
        {
            this->m_pObject = other.m_pObject;
            other.m_pObject = nullptr;
            return *this;
        }

        T& operator *  () const { return *m_pObject; }
        T* operator -> () const { return  m_pObject; }
        T* get() const { return m_pObject; }

        bool operator == ( const CRawPointer& other ) const { return m_pObject == other.m_pObject; }
        bool operator != ( const CRawPointer& other ) const { return m_pObject != other.m_pObject; }

        bool operator == ( T *pOther ) const { return m_pObject == pOther; }
        bool operator != ( T *pOther ) const { return m_pObject == pOther; }

        bool operator == ( std::nullptr_t ) const { return m_pObject == nullptr; }
        bool operator != ( std::nullptr_t ) const { return m_pObject != nullptr; }
    private:
        T* m_pObject = nullptr;
    };

    template <typename WaitableType = CRawPointer<IWaitable>, size_t MaxEvents = 1024>
    class CAsyncWaiter : private CWaiter<MaxEvents>
    {
    public:
        CAsyncWaiter( const char *pszThreadName )
            : m_Thread{ [cWaiter = this, cName = pszThreadName](){ cWaiter->WaiterThreadFunc(cName); } }
        {
            if constexpr ( UseTracking() )
            {
                m_AddedWaitables.reserve( 32 );
                m_RemovedWaitables.reserve( 32 );
            }
        }

        ~CAsyncWaiter()
        {
            Shutdown();
        }

        void Shutdown()
        {
            CWaiter<MaxEvents>::Shutdown();

            if ( m_Thread.joinable() )
                m_Thread.join();

            if constexpr ( UseTracking() )
            {
                {
                    std::unique_lock lock( m_AddedWaitablesMutex );
                    m_AddedWaitables.clear();
                }

                {
                    std::unique_lock lock( m_RemovedWaitablesMutex );
                    m_RemovedWaitables.clear();
                }
            }
        }

        bool AddWaitable( WaitableType pWaitable, uint32_t nEvents = EPOLLIN | EPOLLHUP )
        {
            if constexpr ( UseTracking() )
            {
                if ( !pWaitable->HasLiveReferences() )
                    return false;

                std::unique_lock lock( m_AddedWaitablesMutex );

                if ( !CWaiter<MaxEvents>::AddWaitable( pWaitable.get(), nEvents ) )
                    return false;

                m_AddedWaitables.emplace_back( pWaitable );
                return true;
            }
            else
            {
                return CWaiter<MaxEvents>::AddWaitable( pWaitable.get(), nEvents );
            }
        }

        void RemoveWaitable( WaitableType pWaitable )
        {
            if constexpr ( UseTracking() )
            {
                if ( !pWaitable->HasLiveReferences() )
                    return;

                std::unique_lock lock( m_RemovedWaitablesMutex );
                m_RemovedWaitables.emplace_back( pWaitable.get() );
            }

            CWaiter<MaxEvents>::RemoveWaitable( pWaitable.get() );
        }

        void WaiterThreadFunc( const char *pszThreadName )
        {
            pthread_setname_np( pthread_self(), pszThreadName );

            while ( this->IsRunning() )
            {
                CWaiter<MaxEvents>::PollEvents();

                if constexpr ( UseTracking() )
                {
                    std::scoped_lock lock( m_AddedWaitablesMutex, m_RemovedWaitablesMutex );
                    for ( auto& pRemoved : m_RemovedWaitables )
                        std::erase( m_AddedWaitables, pRemoved );
                    m_RemovedWaitables.clear();
                }
            }
        }
    private:
        static constexpr bool UseTracking()
        {
            return !std::is_same<WaitableType, CRawPointer<IWaitable>>::value;
        }

        std::thread m_Thread;

        // Avoids bubble in the waiter thread func where lifetimes
        // of objects (eg. shared_ptr) could be too short.
        // Eg. RemoveWaitable but still processing events, or about
        // to start processing events.
        std::mutex m_AddedWaitablesMutex;
        std::vector<WaitableType> m_AddedWaitables;

        std::mutex m_RemovedWaitablesMutex;        
        std::vector<WaitableType> m_RemovedWaitables;
    };


}

