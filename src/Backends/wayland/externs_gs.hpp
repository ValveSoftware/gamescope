#include "backend.h"
#include "convar.h"

#include <memory>

extern gamescope::ConVar<bool> cv_adaptive_sync;
extern gamescope::ConVar<bool> cv_composite_force;
extern gamescope::ConVar<bool> cv_hdr_enabled;

namespace gamescope {

extern std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor();

}
