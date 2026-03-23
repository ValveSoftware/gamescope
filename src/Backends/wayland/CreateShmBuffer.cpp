#include "Utils/Defer.h"
#include "Utils/TempFiles.h"

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace gamescope {

int CreateShmBuffer(uint32_t uSize, void* pData) {
	char szShmBufferPath[PATH_MAX];
	int nFd = MakeTempFile(szShmBufferPath, k_szGamescopeTempShmTemplate);
	if (nFd < 0)
		return -1;

	if (ftruncate(nFd, uSize) < 0) {
		close(nFd);
		return -1;
	}

	if (pData) {
		void* pMappedData = mmap(nullptr, uSize, PROT_READ | PROT_WRITE, MAP_SHARED, nFd, 0);
		if (pMappedData == MAP_FAILED)
			return -1;
		defer(munmap(pMappedData, uSize));

		memcpy(pMappedData, pData, uSize);
	}

	return nFd;
}

} // namespace gamescope
