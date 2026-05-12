#pragma once

#include <cstdint>
#include <wayland-server-core.h>
#include "../Utils/NonCopyable.h"

namespace gamescope::WaylandServer
{

#define WL_PROTO_NULL( ) []<typename... Args>( Args... args ) {}
#define WL_PROTO_DESTROY( )                                                    \
    []<typename... Args>(                                                      \
        wl_client *pClient, wl_resource *pResource, Args... args )             \
    { wl_resource_destroy( pResource ); }
#define WL_PROTO( type, name )                                                 \
    []<typename... Args>(                                                      \
        wl_client *pClient, wl_resource *pResource, Args... args )             \
    {                                                                          \
        type *pThing = reinterpret_cast<type *>(                               \
            wl_resource_get_user_data( pResource ) );                          \
        pThing->name( std::forward<Args>( args )... );                         \
    }

#define WL_PROTO_DEFINE( type, version )                                       \
    static constexpr uint32_t                   Version   = version;           \
    static constexpr const struct wl_interface *Interface = &type##_interface; \
    static const struct type##_interface        Implementation;

#define WL_PROTO_DEFAULT_CONSTRUCTOR( )                                        \
    using CWaylandResource::CWaylandResource;

    struct WaylandResourceDesc_t
    {
        wl_client   *pClient;
        wl_resource *pResource;
        uint32_t     uVersion;
    };

    class CWaylandResource : public NonCopyable
    {
    public:
        CWaylandResource( WaylandResourceDesc_t desc ) :
            m_pClient{ desc.pClient },
            m_pResource{ desc.pResource },
            m_uVersion{ desc.uVersion }
        {}

        ~CWaylandResource( ) {}

        template<typename T>
        static bool CheckAllocation( const T &object, wl_client *pClient )
        {
            if ( !object )
            {
                wl_client_post_no_memory( pClient );
                return false;
            }

            return true;
        }

        template<typename T> bool CheckAllocation( const T &object )
        { return CheckAllocation( object, m_pClient ); }

        template<typename T> static T *FromWlResource( wl_resource *pResource )
        {
            T *pObject =
                reinterpret_cast<T *>( wl_resource_get_user_data( pResource ) );
            return pObject;
        }

        template<typename T, typename... Args>
        static T *Create(
            wl_client *pClient, uint32_t uVersion, uint32_t uId, Args... args )
        {
            wl_resource *pResource =
                wl_resource_create( pClient, T::Interface, uVersion, uId );
            if ( !CheckAllocation( pResource, pClient ) ) return nullptr;

            WaylandResourceDesc_t desc = {
                .pClient   = pClient,
                .pResource = pResource,
                .uVersion  = uVersion,
            };
            T *pThing = new T{ desc, std::forward<Args>( args )... };
            if ( !CheckAllocation( pThing, pClient ) ) return nullptr;

            wl_resource_set_implementation(
                pResource,
                &T::Implementation,
                pThing,
                []( wl_resource *pResource )
                {
                    T *pObject =
                        CWaylandResource::FromWlResource<T>( pResource );
                    delete pObject;
                } );

            return pThing;
        }

        static uint64_t ToUint64( uint32_t uHi, uint32_t uLo )
        { return ( uint64_t( uHi ) << 32 ) | uLo; }

        wl_client   *GetClient( ) const { return m_pClient; }
        wl_resource *GetResource( ) const { return m_pResource; }
        uint32_t     GetVersion( ) const { return m_uVersion; }

    protected:
        wl_client   *m_pClient   = nullptr;
        wl_resource *m_pResource = nullptr;
        uint32_t     m_uVersion  = 0;
    };

} // namespace gamescope::WaylandServer
