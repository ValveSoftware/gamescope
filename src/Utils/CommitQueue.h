#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace gamescope::CommitQueue
{
	enum class SelectionResult
	{
		Missing,
		Blocked,
		Ready,
	};

	template <typename Queue>
	auto Select( Queue &queue, uint64_t ulCommitID )
	{
		const auto selected = std::find_if( queue.begin(), queue.end(), [ulCommitID]( const auto &commit )
		{
			return commit->commitID == ulCommitID;
		} );
		if ( selected == queue.end() )
			return std::pair{ SelectionResult::Missing, selected };

		// Fence completion order does not imply FIFO submission order.
		const bool blocked = (*selected)->fifo && std::any_of( queue.begin(), selected, [&]( const auto &commit )
		{
			return commit->surf == (*selected)->surf && commit->fifo && !commit->done;
		} );
		return std::pair{ blocked ? SelectionResult::Blocked : SelectionResult::Ready, selected };
	}
}
