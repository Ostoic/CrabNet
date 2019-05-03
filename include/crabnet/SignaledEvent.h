/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  Copyright (c) 2016-2018, TES3MP Team
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifndef __SIGNALED_EVENT_H
#define __SIGNALED_EVENT_H


#if   defined(_WIN32)
#include "WindowsIncludes.h"

#else
    #include <pthread.h>
    #include <sys/types.h>
#include <atomic>
    #include "SimpleMutex.h"
#endif

#include "Export.h"

namespace CrabNet
{

    class RAK_DLL_EXPORT SignaledEvent
    {
    public:
        SignaledEvent();
        ~SignaledEvent();

        void CloseEvent();
        void SetEvent();
        void WaitOnEvent(int timeoutMs);

    protected:
#ifdef _WIN32
        HANDLE eventList;
#else
        std::atomic_bool isSignaled;
#if !defined(ANDROID)
        pthread_condattr_t condAttr;
#endif
        pthread_cond_t eventList;
        pthread_mutex_t hMutex;
        pthread_mutexattr_t mutexAttr;
#endif
    private:
        bool closed = false;
    };

} // namespace CrabNet

#endif
