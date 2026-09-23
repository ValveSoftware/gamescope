#include <catch2/catch_test_macros.hpp>
#include "Utils/CommitQueue.h"

#include <memory>
#include <vector>

namespace
{
	using gamescope::CommitQueue::Select;
	using gamescope::CommitQueue::SelectionResult;
	struct Commit
	{
		uint64_t commitID;
		uintptr_t surf = 1;
		bool fifo = true;
		bool done = false;
	};
	using Queue = std::vector<std::shared_ptr<Commit>>;
	auto MakeCommit( uint64_t id, uintptr_t surf = 1, bool fifo = true, bool done = false )
	{
		return std::make_shared<Commit>( id, surf, fifo, done );
	}
}

TEST_CASE("Reversed FIFO readiness selects the earlier submission first", "[commit_queue]") {
	Queue queue{ MakeCommit(1), MakeCommit(2) };
	const auto [laterStatus, later] = Select(queue, 2);
	const auto [earlierStatus, earlier] = Select(queue, 1);
	REQUIRE(laterStatus == SelectionResult::Blocked);
	REQUIRE(earlierStatus == SelectionResult::Ready);
	REQUIRE((*earlier)->commitID == 1);
	REQUIRE_FALSE((*later)->done);

	(*earlier)->done = true;
	const auto [nextStatus, next] = Select(queue, 2);
	REQUIRE(nextStatus == SelectionResult::Ready);
	REQUIRE((*next)->commitID == 2);
}

TEST_CASE("FIFO dependencies stay within the submitting surface", "[commit_queue]") {
	Queue queue{ MakeCommit(1, 1), MakeCommit(2, 2) };
	const auto [status, selected] = Select(queue, 2);
	REQUIRE(status == SelectionResult::Ready);
	REQUIRE((*selected)->commitID == 2);
}

TEST_CASE("Mailbox frames can supersede unfinished FIFO frames", "[commit_queue]") {
	Queue queue{ MakeCommit(1), MakeCommit(2, 1, false) };
	REQUIRE(Select(queue, 2).first == SelectionResult::Ready);
}

TEST_CASE("Finished FIFO and mailbox predecessors do not block FIFO", "[commit_queue]") {
	Queue queue{ MakeCommit(1, 1, true, true), MakeCommit(2, 1, false), MakeCommit(3) };
	REQUIRE(Select(queue, 3).first == SelectionResult::Ready);
}

TEST_CASE("An unfinished same-surface FIFO predecessor still blocks past other surfaces", "[commit_queue]") {
	Queue queue{ MakeCommit(1, 1), MakeCommit(2, 2), MakeCommit(3, 1) };
	REQUIRE(Select(queue, 3).first == SelectionResult::Blocked);
	queue[0]->done = true;
	REQUIRE(Select(queue, 3).first == SelectionResult::Ready);
}

TEST_CASE("Empty queues and stale completion ids have no selection", "[commit_queue]") {
	Queue queue;
	REQUIRE(Select(queue, 1).first == SelectionResult::Missing);
	queue.push_back(MakeCommit(2));
	const auto [status, selected] = Select(queue, 1);
	REQUIRE(status == SelectionResult::Missing);
	REQUIRE(selected == queue.end());
}
