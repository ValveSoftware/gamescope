#pragma once

#ifdef __clang__
# define FAST_MATH_ON _Pragma("float_control(push)");        \
                     _Pragma("float_control(precise, off)") //https://clang.llvm.org/docs/LanguageExtensions.html#extensions-to-specify-floating-point-flags
# define FAST_MATH_OFF _Pragma("float_control(pop)")

#elif defined(__GNUC__)
# define FAST_MATH_ON  _Pragma("GCC push_options");             \
                      _Pragma("GCC optimize(\"-ffast-math\")")
# define FAST_MATH_OFF _Pragma("GCC pop_options")

#else
# define FAST_MATH_ON
# define FAST_MATH_OFF
#endif
