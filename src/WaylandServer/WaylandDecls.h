#pragma once

namespace gamescope::WaylandServer
{
    class CWaylandResource;

    template <typename... Types>
	class CWaylandProtocol;

    class CLinuxDrmSyncobjManager;
    class CLinuxDrmSyncobjSurface;
    class CLinuxDrmSyncobjTimeline;
    using CLinuxDrmSyncobj = CWaylandProtocol<CLinuxDrmSyncobjManager>;

    class CReshadeManager;
    using CReshade = CWaylandProtocol<CReshadeManager>;

}
