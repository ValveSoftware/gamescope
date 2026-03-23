#pragma once

#include <cstddef>
#include <tuple>
#include <utility>

// Libdecor puts its userdata ptr at the end... how fun! I shouldn't have spent so long writing this
// total atrocity to mankind.
#define LIBDECOR_USERDATA_TO_THIS(type, name)                                                      \
	[]<typename... Args>(Args... args) {                                                           \
		type* pThing = (type*)std::get<sizeof...(Args) - 1>(std::forward_as_tuple(args...));       \
		CallWithAllButLast(                                                                        \
			[&]<typename... Args2>(Args2... args2) {                                               \
				pThing->name(std::forward<Args2>(args2)...);                                       \
			},                                                                                     \
			std::forward<Args>(args)...                                                            \
		);                                                                                         \
	}

template<typename Func, typename... Args>
auto CallWithAllButLast(Func pFunc, Args&&... args) {
	auto Forwarder = [&]<typename Tuple, size_t... idx>(
						 Tuple&& tuple, std::index_sequence<idx...>
					 ) { return pFunc(std::get<idx>(std::forward<Tuple>(tuple))...); };
	return Forwarder(
		std::forward_as_tuple(args...), std::make_index_sequence<sizeof...(Args) - 1>()
	);
}
