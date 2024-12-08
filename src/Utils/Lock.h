#pragma once
#include "Utils/NonCopyable.h"
#include <memory>
#include <mutex>
#include <optional>
#include <cassert>

namespace gamescope
{
	class CGlobalLockReference;
	template <CGlobalLockReference const&>
	class ReferencedLock;
	class CGlobalLockReference : public NonCopyable
	{
		template <CGlobalLockReference const&>
		friend class ReferencedLock;
		public:
			inline std::optional<std::unique_lock<std::mutex>> popLock() const;
			CGlobalLockReference() = default;
			CGlobalLockReference(CGlobalLockReference&&) = delete;
			CGlobalLockReference(CGlobalLockReference const&&) = delete;
		protected:
			mutable std::optional<std::unique_lock<std::mutex>>* m_pLock;
			mutable std::thread::id m_owningThreadID;
	};
	
	template <CGlobalLockReference const& StaticGlobLoc> //a reference to a static object is actually constexpr, which is nice, because putting the reference-to-static in the template param means this class takes up a bit less space
	class ReferencedLock : public NonCopyable
	{
		using T = std::unique_lock<std::mutex>;
		friend class CGlobalLockReference;
		public:
			explicit ReferencedLock(std::mutex& mut) : m_lock{std::in_place_t{}, std::unique_lock{mut}} 
			{
				StaticGlobLoc.m_pLock = &(this->m_lock);
				StaticGlobLoc.m_owningThreadID = std::this_thread::get_id();
			}

			//should still be able to move ReferencedLock when move elision can be done,
			//but otherwise don't want to risk messing something up
			ReferencedLock(ReferencedLock&&) = delete;
			ReferencedLock(ReferencedLock const&&) = delete;
			
			~ReferencedLock() {
				if (m_lock.has_value()) {
					StaticGlobLoc.m_pLock = nullptr;
				}
			}
			inline std::optional<T> popLock() {
				StaticGlobLoc.m_pLock = nullptr;
				return std::move(m_lock);
			}
		protected:
			mutable std::optional<T> m_lock;
	};
	
	std::optional<std::unique_lock<std::mutex>> CGlobalLockReference::popLock() const {
		assert(m_owningThreadID == std::this_thread::get_id());
		
		if (m_pLock) {
			return std::move(*std::exchange(m_pLock,nullptr));
		} else {
			return std::nullopt;
		}
	}
}