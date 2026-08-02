#pragma once

#define WAYLAND_NULL() []<typename... Args>(void* pData, Args... args) {}
#define WAYLAND_USERDATA_TO_THIS(type, name)                                                       \
	[]<typename... Args>(void* pData, Args... args) {                                              \
		type* pThing = (type*)pData;                                                               \
		pThing->name(std::forward<Args>(args)...);                                                 \
	}
