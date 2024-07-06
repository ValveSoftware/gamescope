#pragma once

#include "waitable.h"

struct eis;

namespace gamescope
{
    class GamescopeInputServer final : public IWaitable
    {
    public:
        GamescopeInputServer();
        ~GamescopeInputServer();

        bool Init( const char *pszSocketPath );

        virtual int GetFD() override;
        virtual void OnPollIn() override;
    private:
        eis *m_pEis = nullptr;
        int m_nFd = -1;

        double m_flScrollAccum[2]{};
    };
}
