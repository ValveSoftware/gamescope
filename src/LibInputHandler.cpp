#include "LibInputHandler.h"

#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include "log.hpp"
#include "backend.h"
#include "wlserver.hpp"
#include "Utils/Defer.h"

// Handles libinput in contexts where we don't have a session
// and can't use the wlroots libinput stuff.
//
// eg. in VR where we want global access to the m + kb
// without doing any seat dance.
//
// That may change in the future...
// but for now, this solves that problem.

namespace gamescope
{
    static LogScope log_input_stealer( "InputStealer" );

    const libinput_interface CLibInputHandler::s_LibInputInterface =
    {
        .open_restricted = []( const char *pszPath, int nFlags, void *pUserData ) -> int
        {
            return open( pszPath, nFlags );
        },

        .close_restricted = []( int nFd, void *pUserData ) -> void
        {
            close( nFd );
        },
    };

    CLibInputHandler::CLibInputHandler()
    {
    }

    CLibInputHandler::~CLibInputHandler()
    {
        if ( m_pLibInput )
        {
            libinput_unref( m_pLibInput );
            m_pLibInput = nullptr;
        }

        if ( m_pUdev )
        {
            udev_unref( m_pUdev );
            m_pUdev = nullptr;
        }
    }

    bool CLibInputHandler::Init()
    {
        m_pUdev = udev_new();
        if ( !m_pUdev )
        {
            log_input_stealer.errorf( "Failed to create udev interface" );
            return false;
        }

        m_pLibInput = libinput_udev_create_context( &s_LibInputInterface, nullptr, m_pUdev );
        if ( !m_pLibInput )
        {
            log_input_stealer.errorf( "Failed to create libinput context" );
            return false;
        }

        const char *pszSeatName = "seat0";
        if ( libinput_udev_assign_seat( m_pLibInput, pszSeatName ) == -1 )
        {
            log_input_stealer.errorf( "Could not assign seat \"%s\"", pszSeatName );
            return false;
        }

        return true;
    }

    int CLibInputHandler::GetFD()
    {
        if ( !m_pLibInput )
            return -1;

        return libinput_get_fd( m_pLibInput );
    }

    void CLibInputHandler::OnPollIn()
    {
        static uint32_t s_uSequence = 0;

        libinput_dispatch( m_pLibInput );

		while ( libinput_event *pEvent = libinput_get_event( m_pLibInput ) )
        {
            defer( libinput_event_destroy( pEvent ) );

            libinput_event_type eEventType = libinput_event_get_type( pEvent );

            switch ( eEventType )
            {
                case LIBINPUT_EVENT_POINTER_MOTION:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    double flDx = libinput_event_pointer_get_dx( pPointerEvent );
                    double flDy = libinput_event_pointer_get_dy( pPointerEvent );

                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    wlserver_lock();
                    wlserver_mousemotion( flDx, flDy, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    double flX = libinput_event_pointer_get_absolute_x( pPointerEvent );
                    double flY = libinput_event_pointer_get_absolute_y( pPointerEvent );

                    GetBackend()->NotifyPhysicalInput( InputType::Mouse );

                    wlserver_lock();
                    wlserver_mousewarp( flX, flY, ++s_uSequence, true );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_BUTTON:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    uint32_t uButton = libinput_event_pointer_get_button( pPointerEvent );
                    libinput_button_state eButtonState = libinput_event_pointer_get_button_state( pPointerEvent );

                    wlserver_lock();
                    wlserver_mousebutton( uButton, eButtonState == LIBINPUT_BUTTON_STATE_PRESSED, ++s_uSequence );
                    wlserver_unlock();
                }
                break;

                case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    static constexpr libinput_pointer_axis eAxes[] =
                    {
                        LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
                        LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                    };

                    for ( uint32_t i = 0; i < std::size( eAxes ); i++ )
                    {
                        libinput_pointer_axis eAxis = eAxes[i];

                        if ( !libinput_event_pointer_has_axis( pPointerEvent, eAxis ) )
                            continue;

                        double flScroll = libinput_event_pointer_get_scroll_value_v120( pPointerEvent, eAxis );
                        m_flScrollAccum[i] += flScroll / 120.0;
                    }
                }
                break;

                case LIBINPUT_EVENT_KEYBOARD_KEY:
                {
                    libinput_event_keyboard *pKeyboardEvent = libinput_event_get_keyboard_event( pEvent );
                    uint32_t uKey = libinput_event_keyboard_get_key( pKeyboardEvent );
                    libinput_key_state eState = libinput_event_keyboard_get_key_state( pKeyboardEvent );

                    wlserver_lock();
                wlserver_key( uKey, eState == LIBINPUT_KEY_STATE_PRESSED, ++    s_uSequence );
                    wlserver_unlock();
                }
                break;

                default:
                    break;
            }
		}

        // Handle scrolling
        {
            double flScrollX = m_flScrollAccum[0];
            double flScrollY = m_flScrollAccum[1];
            m_flScrollAccum[0] = 0.0;
            m_flScrollAccum[1] = 0.0;

            if ( flScrollX != 0.0 || flScrollY != 0.0 )
            {
                wlserver_lock();
                wlserver_mousewheel( flScrollX, flScrollY, ++s_uSequence );
                wlserver_unlock();
            }
        }
    }
}
