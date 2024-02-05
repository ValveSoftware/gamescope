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
                        g_WaitableLog.errorf_errno( "Failed to drain CNudgeWaitable" );
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
        CFunctionWaitable( int nFD, std::function<void()> fnPollFunc )
            : m_nFD{ nFD }
            , m_fnPollFunc{ fnPollFunc }
        {
        }

        void OnPollIn() final
        {
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

        void PollEvents( int nTimeOut = -1 )
        {
            epoll_event events[MaxEvents];

            for ( ;; )
            {
                int nEventCount = epoll_wait( m_nEpollFD, events, MaxEvents, nTimeOut );

                if ( !m_bRunning )
                    return;

                if ( nEventCount < 0 )
                {
                    if ( errno == EAGAIN )
                        continue;

                    g_WaitableLog.errorf_errno( "Failed to epoll_wait in CAsyncWaiter" );
                    return;
                }

                for ( int i = 0; i < nEventCount; i++ )
                {
                    epoll_event &event = events[i];

                    IWaitable *pWaitable = reinterpret_cast<IWaitable *>( event.data.ptr );
                    pWaitable->HandleEvents( event.events );
                }

                return;
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

    template <typename GCWaitableType = IWaitable*, size_t MaxEvents = 1024>
    class CAsyncWaiter : public CWaiter<MaxEvents>
    {
    public:
        CAsyncWaiter( const char *pszThreadName )
            : m_Thread{ [cWaiter = this, cName = pszThreadName](){ cWaiter->WaiterThreadFunc(cName); } }
        {
            m_WaitableGarbageCollector.reserve( 32 );
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

            std::unique_lock lock( m_WaitableGCMutex );
            m_WaitableGarbageCollector.clear();
        }

        void GCWaitable( GCWaitableType GCWaitable, IWaitable *pWaitable )
        {
            std::unique_lock lock( m_WaitableGCMutex );

            m_WaitableGarbageCollector.emplace_back( std::move( GCWaitable ) );

            this->RemoveWaitable( pWaitable );
            this->Nudge();
        }

        void WaiterThreadFunc( const char *pszThreadName )
        {
            pthread_setname_np( pthread_self(), pszThreadName );

            while ( this->IsRunning() )
            {
                CWaiter<MaxEvents>::PollEvents();

                std::unique_lock lock( m_WaitableGCMutex );
                m_WaitableGarbageCollector.clear();
            }
        }
    private:
        std::thread m_Thread;

        // Avoids bubble in the waiter thread func where lifetimes
        // of objects (eg. shared_ptr) could be too short.
        // Eg. RemoveWaitable but still processing events, or about
        // to start processing events.
        std::mutex m_WaitableGCMutex;
        std::vector<GCWaitableType> m_WaitableGarbageCollector;
    };


}

