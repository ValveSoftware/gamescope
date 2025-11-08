#include "LibInputHandler.h"

#include <cstddef>
#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input.h>


#include "log.hpp"
#include "backend.h"
#include "main.hpp"
#include "wlserver.hpp"
#include "Utils/Defer.h"

#include <linux/input-event-codes.h>

// Handles libinput in contexts where we don't have a session
// and can't use the wlroots libinput stuff.
//
// eg. in VR where we want global access to the m + kb
// without doing any seat dance.
//
// Used in SDL & Wayland backend when using libinput held devices
// to prevent handling by base system while still interacting with gamescope.
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
            if (g_libinputSelectedDevices.size() > 0) {
                int dev_fd = open( pszPath, nFlags );
                
                if (dev_fd == -1) {
                    log_input_stealer.errorf( "Failed to open device: %s", pszPath);
                    return -1;
                } 
                
                if (g_bGrabbed) {
                    if (ioctl(dev_fd, EVIOCGRAB, 1) < 0) {
                        // Do not close here, we can continue with it not exclusive.
                        log_input_stealer.warnf( "Failed to grab exclusive lock on device: %s", pszPath);
                    }
                }

                g_libinputSelectedDevices_grabbed_fds.push_back(dev_fd);

                return dev_fd;
            }

            return open( pszPath, nFlags );
        },

        .close_restricted = []( int nFd, void *pUserData ) -> void
        {
            if (g_libinputSelectedDevices.size() > 0) {
                if (g_bGrabbed) {
                    if (ioctl(nFd, EVIOCGRAB, 0) < 0) {
                        log_input_stealer.warnf("Failed to release exclusive grab");
                    }
                }
                g_libinputSelectedDevices_grabbed_fds.erase(
                    std::remove(
                        g_libinputSelectedDevices_grabbed_fds.begin(),
                        g_libinputSelectedDevices_grabbed_fds.end(),
                        nFd
                    )
                );
            }

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


        if (g_libinputSelectedDevices.size() > 0) {
            m_pLibInput = libinput_path_create_context(&s_LibInputInterface, nullptr);

            if ( !m_pLibInput )
            {
                log_input_stealer.errorf( "Failed to create libinput context" );
                return false;
            }

            
            for (std::string dev_path: g_libinputSelectedDevices) {
                if (
                    libinput_path_add_device(m_pLibInput, dev_path.c_str()) == NULL
                ) {
                    log_input_stealer.errorf( "Failed to create libinput device: %s", dev_path.c_str());
                    return false;
                }
            }
            libinput_resume(m_pLibInput);

        } else {


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
                    if (!g_bGrabbed && g_libinputSelectedDevices.size() > 0) continue; // Dont propogate inputs from virtual devices if we dont hold them.
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
                    if (!g_bGrabbed && g_libinputSelectedDevices.size() > 0) continue; // Dont propogate inputs from virtual devices if we dont hold them.
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
                    if (!g_bGrabbed && g_libinputSelectedDevices.size() > 0) continue; // Dont propogate inputs from virtual devices if we dont hold them.
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
                    if (!g_bGrabbed && g_libinputSelectedDevices.size() > 0) continue; // Dont propogate inputs from virtual devices if we dont hold them.
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
                    static std::bitset<KEY_MAX+1> held_keys;
                    static bool toggle_grab_on_all_keys_up = false;

                    if (uKey <= KEY_MAX) {
                        held_keys[uKey] = (eState == LIBINPUT_KEY_STATE_PRESSED); // Toggle the pressed button
                    }

                    if (held_keys[KEY_G] && held_keys[KEY_LEFTMETA]) toggle_grab_on_all_keys_up = true;

                    if (toggle_grab_on_all_keys_up && held_keys.none()) {
                        toggle_grab_on_all_keys_up = false;
                        g_bGrabbed = !g_bGrabbed;

                        for (int dev_fd : g_libinputSelectedDevices_grabbed_fds) {
                            if (g_bGrabbed) {
                                if (ioctl(dev_fd, EVIOCGRAB, 1) < 0) {
                                    fprintf( stderr,"Libinput: Failed to grab exclusive lock on device: %d\n", dev_fd);
                                }
                            } else {
                                if (ioctl(dev_fd, EVIOCGRAB, 0) < 0) {
                                    fprintf( stderr,"Libinput: Failed to release exclusive grab on device: %d\n", dev_fd);
                                }
                            }
                        }
                    } else { // Pass through key release just because if we dont, it would be held.
                        if (!g_bGrabbed && g_libinputSelectedDevices.size() > 0) continue; // Dont propogate inputs from virtual devices if we dont hold them.
                    }

                    
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
