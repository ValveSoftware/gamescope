#pragma once
#include <exception>
#include "tracy/Tracy.hpp"


// (from 3.4.6 of Tracy manual) Tracy doesn't support exiting within a zone.
// workaround is to throw a custom exception,
// then catch exception at the end of the function

inline constexpr const char* const sl_steamcompmgr_name = "gamescope-xwm";

#ifdef TRACY_ENABLE
#	define EXIT(status) throw ETracyExit(status)
#	define PTHREAD_EXIT(status) throw ETracyExit(status, true)
#	define MAYBE_NORETURN
#	define TRACY_TRY try
#	define TRACY_CATCH catch(const ETracyExit& e) { e.m_bPthreadExit ? pthread_exit(const_cast<void*>(e.m_pStatus)) : exit(e.m_status); }
#else
#	define EXIT(status) exit(status)
#	define PTHREAD_EXIT(status) pthread_exit(status)
#	define MAYBE_NORETURN [[noreturn]]
#	define TRACY_TRY
#	define TRACY_CATCH 
#endif

#ifdef TRACY_ENABLE
struct ETracyExit : public std::exception {
		const int m_status;
		const void * const m_pStatus;
		const bool m_bPthreadExit;
		ETracyExit(void* pStatus, bool bPthreadExit) : m_status{0}, m_pStatus{pStatus}, m_bPthreadExit{bPthreadExit} {}
		ETracyExit(int status) : m_status{status}, m_pStatus{nullptr}, m_bPthreadExit{false} {} 
		constexpr const char * what () {
		    return "Custom exception for safely exiting from within a Tracy profiler zone";
		}
};
#endif