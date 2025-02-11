//
// MIT License
// Copyright (c) 2019 Jonathan R. Madsen
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
// "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//  ---------------------------------------------------------------
//  Tasking class header
//  Class Description:
//  ---------------------------------------------------------------
//  Author: Jonathan Madsen
//  ---------------------------------------------------------------

#pragma once

#include <cstdint>
#include <deque>
#include <memory>

//--------------------------------------------------------------------------------------//

#if defined(PTL_USE_TBB)

#    include <tbb/task_group.h>
#    include <tbb/task_scheduler_init.h>

typedef tbb::task_group          tbb_task_group_t;
typedef tbb::task_scheduler_init tbb_task_scheduler_t;

#else

namespace tbb
{
class task_group
{
public:
    // dummy constructor
    task_group() {}
    // dummy wait
    inline void wait() {}
    // run function
    template <typename _Func>
    inline void run(_Func f)
    {
        f();
    }
    // run and wait
    template <typename _Func>
    inline void run_and_wait(_Func f)
    {
        f();
    }
};

class task_scheduler_init
{
public:
    // dummy constructor
    task_scheduler_init(int = 0, uint64_t = 0) {}
    // dummy initialize
    inline void initialize(int = 0) {}
};

}  // namespace tbb

typedef tbb::task_group          tbb_task_group_t;
typedef tbb::task_scheduler_init tbb_task_scheduler_t;

#endif

//--------------------------------------------------------------------------------------//

class ThreadPool;
class VUserTaskQueue;

//--------------------------------------------------------------------------------------//

class ThreadData
{
public:
    using pointer_t = std::unique_ptr<ThreadData>;

    template <typename _Tp>
    using TaskStack = std::deque<_Tp>;

    ThreadData();
    ThreadData(ThreadPool* tp);
    ~ThreadData();

    ThreadData& operator=(ThreadPool*);

public:
    bool                       is_master     = false;
    bool                       within_task   = false;
    intmax_t                   task_depth    = 0;
    ThreadPool*                thread_pool   = nullptr;
    VUserTaskQueue*            current_queue = nullptr;
    TaskStack<VUserTaskQueue*> queue_stack;

public:
    // Public functions
    static pointer_t& GetInstance();
};

//--------------------------------------------------------------------------------------//
