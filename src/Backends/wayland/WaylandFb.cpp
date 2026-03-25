#include "WaylandFb.hpp"

#include "callback_macro.hpp"

namespace gamescope {

const wl_buffer_listener CWaylandFb::s_BufferListener = {
	.release = WAYLAND_USERDATA_TO_THIS(CWaylandFb, Wayland_Buffer_Release),
};

}
