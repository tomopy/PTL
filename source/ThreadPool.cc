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
// ---------------------------------------------------------------
//  Tasking class implementation
//
// Class Description:
//
// This file creates a class for an efficient thread-pool that
// accepts work in the form of tasks.
//
// ---------------------------------------------------------------
// Author: Jonathan Madsen (Feb 13th 2018)
// ---------------------------------------------------------------

#include "PTL/ThreadPool.hh"
#include "PTL/Globals.hh"
#include "PTL/ThreadData.hh"
#include "PTL/UserTaskQueue.hh"
#include "PTL/VUserTaskQueue.hh"

#include <cstdlib>

#if defined(PTL_USE_GPERF)
#    include <gperftools/heap-checker.h>
#    include <gperftools/heap-profiler.h>
#    include <gperftools/profiler.h>
#endif

//======================================================================================//

inline intmax_t
ncores()
{
    return static_cast<intmax_t>(Thread::hardware_concurrency());
}

//======================================================================================//

ThreadPool::thread_id_map_t ThreadPool::f_thread_ids;

//======================================================================================//

namespace
{
ThreadData::pointer_t&
thread_data()
{
    return ThreadData::GetInstance();
}
}

//======================================================================================//

bool ThreadPool::f_use_tbb = false;

//======================================================================================//
// static member function that calls the member function we want the thread to
// run
void
ThreadPool::start_thread(ThreadPool* tp, intmax_t _idx)
{
    {
        AutoLock lock(TypeMutex<ThreadPool>(), std::defer_lock);
        if(!lock.owns_lock())
            lock.lock();
        if(_idx < 0)
            _idx = f_thread_ids.size();
        f_thread_ids[std::this_thread::get_id()] = _idx;
    }
    thread_data().reset(new ThreadData(tp));
    tp->execute_thread(thread_data()->current_queue);
}

//======================================================================================//
// static member function that initialized tbb library
void
ThreadPool::set_use_tbb(bool enable)
{
#if defined(PTL_USE_TBB)
    f_use_tbb = enable;
#else
    ConsumeParameters<bool>(enable);
#endif
}

//======================================================================================//

uintmax_t
ThreadPool::GetThisThreadID()
{
    auto _tid = ThisThread::get_id();
    {
        AutoLock lock(TypeMutex<ThreadPool>(), std::defer_lock);
        if(!lock.owns_lock())
            lock.lock();
        if(f_thread_ids.find(_tid) == f_thread_ids.end())
        {
            auto _idx          = f_thread_ids.size();
            f_thread_ids[_tid] = _idx;
        }
    }
    return f_thread_ids[_tid];
}

//======================================================================================//

ThreadPool::ThreadPool(const size_type& pool_size, VUserTaskQueue* task_queue,
                       bool _use_affinity, const affinity_func_t& _affinity_func)
: m_use_affinity(_use_affinity)
, m_tbb_tp(false)
, m_alive_flag(false)
, m_verbose(0)
, m_pool_size(0)
, m_pool_state(thread_pool::state::NONINIT)
, m_master_tid(ThisThread::get_id())
, m_thread_awake(new atomic_int_type(0))
, m_task_queue(task_queue)
, m_tbb_task_group(nullptr)
, m_init_func([]() { return; })
, m_affinity_func(_affinity_func)
{
    m_verbose = GetEnv<int>("PTL_VERBOSE", m_verbose);

    if(!m_task_queue)
        m_task_queue = new UserTaskQueue(pool_size);

    auto master_id = GetThisThreadID();
    if(master_id != 0 && m_verbose > 1)
        std::cerr << "ThreadPool created on non-master slave" << std::endl;

    thread_data().reset(new ThreadData(this));

    // initialize after GetThisThreadID so master is zero
    this->initialize_threadpool(pool_size);
}

//======================================================================================//

ThreadPool::~ThreadPool()
{
    if(m_alive_flag.load())
        destroy_threadpool();
}

//======================================================================================//

bool
ThreadPool::is_initialized() const
{
    return !(m_pool_state.load() == thread_pool::state::NONINIT);
}

//======================================================================================//

void
ThreadPool::set_affinity(intmax_t i, Thread& _thread)
{
    try
    {
        NativeThread native_thread = _thread.native_handle();
        intmax_t     _pin          = m_affinity_func(i);
        if(m_verbose > 0)
        {
            std::cout << "Setting pin affinity for thread " << _thread.get_id() << " to "
                      << _pin << std::endl;
        }
        Threading::SetPinAffinity(_pin, native_thread);
    }
    catch(std::runtime_error& e)
    {
        std::cout << "Error setting pin affinity" << std::endl;
        std::cerr << e.what() << std::endl;  // issue assigning affinity
    }
}

//======================================================================================//

ThreadPool::size_type
ThreadPool::initialize_threadpool(size_type proposed_size)
{
    //--------------------------------------------------------------------//
    // return before initializing
    if(proposed_size < 1)
        return 0;

    //--------------------------------------------------------------------//
    // store that has been started
    if(!m_alive_flag.load())
        m_pool_state.store(thread_pool::state::STARTED);

        //--------------------------------------------------------------------//
        // handle tbb task scheduler
#ifdef PTL_USE_TBB
    if(f_use_tbb)
    {
        m_tbb_tp                               = true;
        m_pool_size                            = proposed_size;
        tbb_task_scheduler_t*& _task_scheduler = tbb_task_scheduler();
        // delete if wrong size
        if(m_pool_size != proposed_size)
        {
            delete _task_scheduler;
            _task_scheduler = nullptr;
        }

        if(!_task_scheduler)
        {
            _task_scheduler =
                new tbb_task_scheduler_t(tbb::task_scheduler_init::deferred);
        }

        if(!_task_scheduler->is_active())
        {
            m_pool_size = proposed_size;
            _task_scheduler->initialize(proposed_size + 1);
            if(m_verbose > 0)
            {
                std::cout << "ThreadPool [TBB] initialized with " << m_pool_size
                          << " threads." << std::endl;
            }
        }
        // create task group (used for async)
        if(!m_tbb_task_group)
            m_tbb_task_group = new tbb_task_group_t();
        return m_pool_size;
    }

    // NOLINT(readability-else-after-return)
    if(f_use_tbb && tbb_task_scheduler())
    {
        m_tbb_tp                               = false;
        tbb_task_scheduler_t*& _task_scheduler = tbb_task_scheduler();
        if(_task_scheduler)
        {
            _task_scheduler->terminate();
            delete _task_scheduler;
            _task_scheduler = nullptr;
        }
        // delete task group (used for async)
        if(m_tbb_task_group)
        {
            m_tbb_task_group->wait();
            delete m_tbb_task_group;
            m_tbb_task_group = nullptr;
        }
    }
#endif

    m_alive_flag.store(true);

    //--------------------------------------------------------------------//
    // if started, stop some thread if smaller or return if equal
    if(m_pool_state.load() == thread_pool::state::STARTED)
    {
        if(m_pool_size > proposed_size)
        {
            while(stop_thread() > proposed_size)
                ;
            if(m_verbose > 0)
            {
                std::cout << "ThreadPool initialized with " << m_pool_size << " threads."
                          << std::endl;
            }
            return m_pool_size;
        }
        else if(m_pool_size == proposed_size)  // NOLINT
        {
            if(m_verbose > 0)
            {
                std::cout << "ThreadPool initialized with " << m_pool_size << " threads."
                          << std::endl;
            }
            return m_pool_size;
        }
    }

    //--------------------------------------------------------------------//
    // reserve enough space to prevent realloc later
    {
        AutoLock _task_lock(m_task_lock);
        m_is_joined.reserve(proposed_size);
    }

    auto this_tid = GetThisThreadID();
    for(size_type i = m_pool_size; i < proposed_size; ++i)
    {
        // add the threads
        try
        {
            using pointer_t = std::unique_ptr<Thread>;
            auto tid =
                pointer_t(new Thread(ThreadPool::start_thread, this, this_tid + i + 1));
            // only reaches here if successful creation of thread
            ++m_pool_size;
            // store thread
            m_main_threads.push_back(tid->get_id());
            // list of joined thread booleans
            m_is_joined.push_back(false);
            // set the affinity
            if(m_use_affinity)
                set_affinity(i, *tid.get());
            // detach
            m_unique_threads.emplace_back(std::move(tid));
        }
        catch(std::runtime_error& e)
        {
            std::cerr << e.what() << std::endl;  // issue creating thread
            continue;
        }
        catch(std::bad_alloc& e)
        {
            std::cerr << e.what() << std::endl;
            continue;
        }
    }
    //------------------------------------------------------------------------//

    AutoLock _task_lock(m_task_lock);

    // thread pool size doesn't match with join vector
    // this will screw up joining later
    if(m_is_joined.size() != m_main_threads.size())
    {
        std::stringstream ss;
        ss << "ThreadPool::initialize_threadpool - boolean is_joined vector "
           << "is a different size than threads vector: " << m_is_joined.size() << " vs. "
           << m_main_threads.size() << " (tid: " << std::this_thread::get_id() << ")";

        throw std::runtime_error(ss.str());
    }

    if(m_verbose > 0)
    {
        std::cout << "ThreadPool initialized with " << m_pool_size << " threads."
                  << std::endl;
    }
    return m_main_threads.size();
}

//======================================================================================//

ThreadPool::size_type
ThreadPool::destroy_threadpool()
{
    int nid = static_cast<int>(GetThisThreadID());

    // Note: this is not for synchronization, its for thread communication!
    // destroy_threadpool() will only be called from the main thread, yet
    // the modified m_pool_state may not show up to other threads until its
    // modified in a lock!
    //------------------------------------------------------------------------//
    m_pool_state.store(thread_pool::state::STOPPED);

    //------------------------------------------------------------------------//
    // notify all threads we are shutting down
    m_task_lock.lock();
    m_task_cond.notify_all();
    m_task_lock.unlock();

    //--------------------------------------------------------------------//
    // handle tbb task scheduler
#ifdef PTL_USE_TBB
    if(m_tbb_tp && tbb_task_scheduler())
    {
        tbb_task_scheduler_t*& _task_scheduler = tbb_task_scheduler();
        delete _task_scheduler;
        _task_scheduler = nullptr;
        m_tbb_tp        = false;
        std::cout << "ThreadPool [TBB] destroyed" << std::endl;
    }
    if(m_tbb_task_group)
    {
        m_tbb_task_group->wait();
        delete m_tbb_task_group;
        m_tbb_task_group = nullptr;
    }
#endif

    if(!m_alive_flag.load())
        return 0;

    if(m_is_joined.size() != m_main_threads.size())
    {
        std::stringstream ss;
        ss << "   ThreadPool::destroy_thread_pool - boolean is_joined vector "
           << "is a different size than threads vector: " << m_is_joined.size() << " vs. "
           << m_main_threads.size() << " (tid: " << std::this_thread::get_id() << ")";

        throw std::runtime_error(ss.str());
    }

    //--------------------------------------------------------------------//
    // erase thread from thread ID list
    for(auto _tid : m_main_threads)
    {
        if(f_thread_ids.find(_tid) != f_thread_ids.end())
            f_thread_ids.erase(f_thread_ids.find(_tid));
    }

    //--------------------------------------------------------------------//
    // try waking up a bunch of threads that are still waiting
    m_task_cond.notify_all();

    for(auto& itr : m_unique_threads)
        itr->join();

    m_main_threads.clear();
    m_is_joined.clear();
    m_unique_threads.clear();

    m_alive_flag.store(false);

    printf("[%i]> ThreadPool destroyed...\n", nid);

    return 0;
}

//======================================================================================//

ThreadPool::size_type
ThreadPool::stop_thread()
{
    if(!m_alive_flag.load() || m_pool_size == 0)
        return 0;

    //------------------------------------------------------------------------//
    // notify all threads we are shutting down
    m_task_lock.lock();
    m_is_stopped.push_back(true);
    m_task_cond.notify_one();
    m_task_lock.unlock();
    //------------------------------------------------------------------------//

    // lock up the task queue
    AutoLock _task_lock(m_task_lock);

    while(!m_stop_threads.empty())
    {
        auto tid = m_stop_threads.front();
        // remove from stopped
        m_stop_threads.pop_front();
        // remove from main
        for(auto itr = m_main_threads.begin(); itr != m_main_threads.end(); ++itr)
        {
            if(*itr == tid)
            {
                m_main_threads.erase(itr);
                break;
            }
        }
        // remove from join list
        m_is_joined.pop_back();
    }

    m_pool_size = m_main_threads.size();
    return m_main_threads.size();
}

//======================================================================================//

void
ThreadPool::execute_thread(VUserTaskQueue* _task_queue)
{
#if defined(PTL_USE_GPERF)
    ProfilerRegisterThread();
#endif

    // how long the thread waits on condition variable
    // static int wait_time = GetEnv<int>("PTL_POOL_WAIT_TIME", 5);

    ++(*m_thread_awake);

    // initialization function
    m_init_func();

    ThreadId tid  = ThisThread::get_id();
    auto&    data = thread_data();
    // auto        thread_bin = _task_queue->GetThreadBin();
    // auto        workers    = _task_queue->workers();

    assert(data->current_queue != nullptr);
    assert(_task_queue == data->current_queue);

    // essentially a dummy run
    {
        data->within_task = true;
        auto _task        = _task_queue->GetTask();
        if(_task)
        {
            (*_task)();
            if(!_task->group())
                delete _task;
        }
        data->within_task = false;
    }

    // threads stay in this loop forever until thread-pool destroyed
    while(true)
    {
        //--------------------------------------------------------------------//
        // Try to pick a task
        AutoLock _task_lock(m_task_lock, std::defer_lock);
        //--------------------------------------------------------------------//

        auto leave_pool = [&]() {
            auto _state      = [&]() { return static_cast<int>(m_pool_state.load()); };
            auto _pool_state = _state();
            if(_pool_state > 0)
            {
                // stop whole pool
                if(_pool_state == thread_pool::state::STOPPED)
                {
                    return true;
                }
                // single thread stoppage
                else if(_pool_state == thread_pool::state::PARTIAL)  // NOLINT
                {
                    if(!_task_lock.owns_lock())
                        _task_lock.lock();
                    if(!m_is_stopped.empty() && m_is_stopped.back())
                    {
                        m_stop_threads.push_back(tid);
                        m_is_stopped.pop_back();
                        return true;
                    }
                    if(_task_lock.owns_lock())
                        _task_lock.unlock();
                }
            }
            return false;
        };

        // We need to put condition.wait() in a loop for two reasons:
        // 1. There can be spurious wake-ups (due to signal/ENITR)
        // 2. When mutex is released for waiting, another thread can be woken up
        //    from a signal/broadcast and that thread can mess up the condition.
        //    So when the current thread wakes up the condition may no longer be
        //    actually true!
        while(_task_queue->empty())
        {
            auto _state = [&]() { return static_cast<int>(m_pool_state.load()); };
            auto _size  = [&]() { return _task_queue->true_size(); };
            auto _empty = [&]() { return _task_queue->empty(); };
            auto _wake  = [&]() { return (!_empty() || _size() > 0 || _state() > 0); };

            if(leave_pool())
                return;

            if(_task_queue->true_size() == 0)
            {
                if(m_thread_awake && m_thread_awake->load() > 0)
                    --(*m_thread_awake);

                // lock before sleeping on condition
                if(!_task_lock.owns_lock())
                    _task_lock.lock();

                // Wait until there is a task in the queue
                // Unlocks mutex while waiting, then locks it back when signaled
                // use lambda to control waking
                m_task_cond.wait(_task_lock, _wake);

                // leave the pool immediately
                if(leave_pool())
                    return;

                // unlock if owned
                if(_task_lock.owns_lock())
                    _task_lock.unlock();

                // notify that is awake
                if(m_thread_awake && m_thread_awake->load() < m_pool_size)
                    ++(*m_thread_awake);
            }
            else
                break;
        }

        // release the lock
        if(_task_lock.owns_lock())
            _task_lock.unlock();
        //----------------------------------------------------------------//

        // leave pool if conditions dictate it
        if(leave_pool())
            return;
        //----------------------------------------------------------------//

        // activate guard against recursive deadlock
        data->within_task = true;
        //----------------------------------------------------------------//

        // execute the task(s)
        while(!_task_queue->empty())
        {
            auto _task = _task_queue->GetTask();
            if(_task)
            {
                (*_task)();
                if(!_task->group())
                    delete _task;
            }
        }
        //----------------------------------------------------------------//

        // disable guard against recursive deadlock
        data->within_task = false;
        //----------------------------------------------------------------//
    }
}

//======================================================================================//
