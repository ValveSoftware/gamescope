#pragma once

#include "waitable.h"

struct libinput_interface;
struct udev;
struct libinput;

namespace gamescope
{
    class CLibInputHandler final : public IWaitable
    {
    public:
        CLibInputHandler();
        ~CLibInputHandler();

        bool Init();

        virtual int GetFD() override;
        virtual void OnPollIn() override;
    private:
        udev *m_pUdev = nullptr;
        libinput *m_pLibInput = nullptr;

        double m_flScrollAccum[2]{};

        static const libinput_interface s_LibInputInterface;
    };
}