#pragma once

#include <thread>
#include <stdint.h>
#include <sys/epoll.h>

#include "log.hpp"

extern LogScope g_WaitableLog;

namespace gamescope
{
    class IWaitable
    {
    public:
        virtual ~IWaitable() {}

        virtual int GetFD() { return -1; }

        virtual void OnPollIn() {}
        virtual void OnPollOut() {}
        virtual void OnPollHangUp() {}

        void HandleEvents( uint32_t nEvents )
        {
            if ( nEvents & EPOLLIN )
                this->OnPollIn();
            if ( nEvents & EPOLLOUT )
                this->OnPollOut();
            if ( nEvents & EPOLLHUP )
                this->OnPollHangUp();
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
                    close( m_nFDs[i] );
            }
        }

        void Drain()
        {
            if ( m_nFDs[0] < 0 )
                return;

            char buf[1024];
            for (;;)
            {
                if ( read( m_nFDs[0], buf, sizeof( buf ) ) < 0 )
                {
                    if ( errno != EAGAIN )
                        g_WaitableLog.errorf_errno( "Failed to drain CNudgeWaitable" );
                    break;
                }
            }
        }

        bool Nudge()
        {
            return write( m_nFDs[1], "\n", 1 ) >= 0;
        }

        int GetFD() final { return m_nFDs[0]; }
    private:
        int m_nFDs[2] = { -1, -1 };
    };

    template <size_t MaxEvents = 1024>
    class CWaiter
    {
    public:
        CWaiter()
            : m_nEpollFD{ epoll_create1( 0 ) }
        {
            AddWaitable( &m_NudgeWaitable, EPOLLIN );   
        }

        ~CWaiter()
        {
            Shutdown();
        }

        void Shutdown()
        {
            if ( !m_bRunning )
                return;

            Nudge();
            m_bRunning = false;

            if ( m_nEpollFD >= 0 )
            {
                close( m_nEpollFD );
                m_nEpollFD = -1;
            }
        }

        bool AddWaitable( IWaitable *pWaitable, uint32_t nEvents = EPOLLIN | EPOLLOUT | EPOLLHUP )
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

        void PollEvents()
        {
            epoll_event events[MaxEvents];

            int nEventCount = epoll_wait( m_nEpollFD, events, MaxEvents, -1);

            if ( !m_bRunning )
                return;

            if ( nEventCount < 0 )
            {
                g_WaitableLog.errorf_errno( "Failed to epoll_wait in CAsyncWaiter" );
                return;
            }

            for ( int i = 0; i < nEventCount; i++ )
            {
                epoll_event &event = events[i];

                IWaitable *pWaitable = reinterpret_cast<IWaitable *>(event.data.ptr);
                pWaitable->HandleEvents( event.events );
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

    template <size_t MaxEvents = 1024>
    class CAsyncWaiter : public CWaiter<MaxEvents>
    {
    public:
        CAsyncWaiter( const char *pszThreadName )
            : m_Thread{ [cWaiter = this, cName = pszThreadName](){ cWaiter->WaiterThreadFunc(cName); } }
        {
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
        }

        void WaiterThreadFunc( const char *pszThreadName )
        {
            pthread_setname_np( pthread_self(), pszThreadName );

            while ( this->IsRunning() )
                CWaiter<MaxEvents>::PollEvents();
        }
    private:
        std::thread m_Thread;
    };


}

