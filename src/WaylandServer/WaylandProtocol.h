#pragma once

#include <vector>
#include "WaylandResource.h"

namespace gamescope::WaylandServer
{

    template<typename... Types> class CWaylandProtocol : public NonCopyable
    {
    public:
        CWaylandProtocol( wl_display *pDisplay ) : m_pDisplay{ pDisplay }
        { ( CreateGlobal<Types>( ), ... ); }

    private:
        template<typename T> void CreateGlobal( )
        {
            wl_global *pGlobal = wl_global_create(
                m_pDisplay,
                T::Interface,
                T::Version,
                this,
                []( struct wl_client *pClient,
                    void             *pData,
                    uint32_t          uVersion,
                    uint32_t          uId )
                {
                    CWaylandProtocol *pProtocol =
                        reinterpret_cast<CWaylandProtocol *>( pData );
                    pProtocol->Bind<T>( pClient, uVersion, uId );
                } );

            m_pGlobals.emplace_back( pGlobal );
        }

        template<typename T>
        void Bind( struct wl_client *pClient, uint32_t uVersion, uint32_t uId )
        { CWaylandResource::Create<T>( pClient, uVersion, uId ); }

        wl_display              *m_pDisplay = nullptr;
        std::vector<wl_global *> m_pGlobals;
    };

} // namespace gamescope::WaylandServer
