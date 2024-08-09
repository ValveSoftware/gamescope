#pragma once
#ifndef __TRACYAPI_H__
# define __TRACYAPI_H__
# define TRACY_API
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

// Hacky workaround for a compiler error that seems to happen when using tracy::ScopedZone::TextFmt 
// due to gcc being angry that a variadic function (the va_args kind) has the __attribute__((always_inline)) attribute on it
#ifndef __TRACYSCOPED_GAMESCOPE__
# ifdef tracy_force_inline
#  undef tracy_force_inline
# endif

# define tracy_force_inline inline
# ifndef tracy_no_inline
#  define tracy_no_inline __attribute__((noinline))
# endif

# define __TRACYFORCEINLINE_HPP__
# include "client/TracyScoped.hpp"
# undef tracy_force_inline
# define tracy_force_inline __attribute__((always_inline)) inline
# define __TRACYSCOPED_GAMESCOPE__
# define __TRACYSCOPED_HPP__
#endif
//*****************************************//



#include "tracy/TracyVulkan.hpp"

