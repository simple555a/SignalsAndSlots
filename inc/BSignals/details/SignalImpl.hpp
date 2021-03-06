/*
 * File:   SignalImpl.hpp
 * Author: Barath Kannan
 * Signal/Slots C++14 implementation
 * Created on May 10, 2016, 5:57 PM
 */

#ifndef SIGNALIMPL_HPP
#define SIGNALIMPL_HPP

#include <functional>
#include <map>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <utility>
#include <type_traits>

#include "BSignals/details/MPSCQueue.hpp"
#include "BSignals/details/WheeledThreadPool.h"
#include "BSignals/details/Semaphore.h"

namespace BSignals{ namespace details{

//These executors determine how message emission to a slot occurs
    // SYNCHRONOUS:
    // Emission occurs synchronously.
    // When emit returns, all connected slots have been invoked and returned.
    // This method is preferred when connected functions have short execution
    // time, quick emission is required, and/or when it is necessary to know 
    // that the function has returned before proceeding.

    // ASYNCHRONOUS:
    // Emission occurs asynchronously. A detached thread is spawned on emission.
    // When emit returns, the thread has been spawned. The thread automatically
    // destructs when the connected function returns.
    // This method is recommended when connected functions have long execution
    // time and are independent.

    // STRAND:
    // Emission occurs asynchronously. 
    // On connection a dedicated thread (per slot) is spawned to wait for new messages.
    // Emitted parameters are bound to the mapped function and enqueued on the 
    // waiting thread. These messages are then processed synchronously in the
    // spawned thread.
    // This method is recommended when connected functions have longer execution
    // time, the overhead of creating/destroying a thread for each slot would be
    // unperformant, and/or connected functions need to be processed in order 
    // of arrival (FIFO).

    // THREAD POOLED:
    // Emission occurs asynchronously. 
    // On connection, if it is the first thread pooled function by any signal, 
    // the thread pool is initialized with 8 threads, all listening for queued
    // emissions. The number of threads in the pool is not currently run-time
    // configurable but may be in the future.
    // Emitted parameters are bound to the mapped function and enqueued on the 
    // one of the waiting threads. These messages are then processed when the 
    // relevant queue is consumed by the mapped thread pool.
    // This method is recommended when connected functions have longer execution
    // time, the overhead of creating/destroying a thread for each slot would be
    // unperformant, the overhead of a waiting thread for each slot is 
    // unnecessary, and/or connected functions do NOT need to be processed in
    // order of arrival.
//  
enum class ExecutorScheme{
    SYNCHRONOUS,
    ASYNCHRONOUS, 
    STRAND,
    THREAD_POOLED
};
    
template <typename... Args>
class SignalImpl {
public:
    SignalImpl() = default;
    
    SignalImpl(bool enforceThreadSafety) 
        : enableEmissionGuard{enforceThreadSafety} {}
        
    SignalImpl(uint32_t maxAsyncThreads) 
        : sem{maxAsyncThreads} {}
        
    SignalImpl(bool enforceThreadSafety, uint32_t maxAsyncThreads) 
        : enableEmissionGuard{enforceThreadSafety}, sem{maxAsyncThreads} {}
    
    ~SignalImpl(){
        disconnectAllSlots();
    }

    template<typename F, typename C>
    int connectMemberSlot(const ExecutorScheme &scheme, F&& function, C&& instance) const {
        //type check assertions
        static_assert(std::is_member_function_pointer<F>::value, "function is not a member function");
        static_assert(std::is_object<std::remove_reference<C>>::value, "instance is not a class object");
        
        //Construct a bound function from the function pointer and object
        auto boundFunc = objectBind(function, instance);
        return connectSlot(scheme, boundFunc);
    }
    
    int connectSlot(const ExecutorScheme &scheme, std::function<void(Args...)> slot) const {
        std::unique_lock<std::shared_timed_mutex> lock(signalLock);
        uint32_t id = currentId.fetch_add(1);
        auto *slotMap = getSlotMap(scheme);
        slotMap->emplace(id, slot);
        if (scheme == ExecutorScheme::STRAND){
            strandQueues[id];
            strandThreads.emplace(id, std::thread(&SignalImpl::queueListener, this, id));
        }
        else if (scheme == ExecutorScheme::THREAD_POOLED){
            BSignals::details::WheeledThreadPool::startup();
        }
        return (int)id;
    }
    
    void disconnectSlot(const uint32_t &id) const {
        std::unique_lock<std::shared_timed_mutex> lock(signalLock);
        std::map<uint32_t, std::function<void(Args...)>> *slotMap = findSlotMapWithId(id);
        if (slotMap == nullptr) return;
        if (slotMap == &strandSlots){
            strandQueues[id].enqueue(nullptr);
            strandThreads[id].join();
            strandThreads.erase(id);
            strandQueues.erase(id);
        }
        slotMap->erase(id);
    }
    
    void disconnectAllSlots() const { 
        std::unique_lock<std::shared_timed_mutex> lock(signalLock);
        for (auto &q : strandQueues){
            q.second.enqueue(nullptr);
        }
        for (auto &t : strandThreads){
            t.second.join();
        }
        strandThreads.clear();
        strandQueues.clear();
        
        synchronousSlots.clear();
        asynchronousSlots.clear();
        strandSlots.clear();
        threadPooledSlots.clear();
    }
    
    void emitSignal(const Args &... p) const {
        return enableEmissionGuard ? emitSignalThreadSafe(p...) : emitSignalUnsafe(p...);
    }
    
private:
    SignalImpl<Args...>(const SignalImpl<Args...>& that) = delete;
    void operator=(const SignalImpl<Args...>&) = delete;
    
    inline void emitSignalUnsafe(const Args &... p) const {
        for (auto const &slot : synchronousSlots){
            runSynchronous(slot.second, p...);
        }
        
        for (auto const &slot : asynchronousSlots){
            runAsynchronous(slot.second, p...);
        }
        
        for (auto const &slot : strandSlots){
            runStrands(slot.first, slot.second, p...);
        }
        
        for (auto const &slot : threadPooledSlots){
            runThreadPooled(slot.second, p...);
        }
    }
    
    inline void emitSignalThreadSafe(const Args &... p) const {
        std::shared_lock<std::shared_timed_mutex> lock(signalLock);
        emitSignalUnsafe(p...);
    }

    inline void runThreadPooled(const std::function<void(Args...)> &function, const Args &... p) const {
        BSignals::details::WheeledThreadPool::run([&function, p...](){function(p...);});
    }
    
    inline void runAsynchronous(const std::function<void(Args...)> &function, const Args &... p) const {
        sem.acquire();
        std::thread slotThread([this, function, p...](){
            function(p...);
            sem.release();                
        });
        slotThread.detach();
    }
    
    inline void runStrands(uint32_t asyncQueueId, const std::function<void(Args...)> &function, const Args &... p) const{
        //bind the function arguments to the function using a lambda and store
        //the newly bound function. This changes the function signature in the
        //resultant map, there are no longer any parameters in the bound function
        strandQueues[asyncQueueId].enqueue([&function, p...](){function(p...);});
    }
    
    inline void runSynchronous(const std::function<void(Args...)> &function, const Args &... p) const{
        function(p...);
    }
    
    //Reference to instance
    template<typename F, typename I>
    std::function<void(Args...)> objectBind(F&& function, I&& instance) const {
        return[=, &instance](Args... args){
            (instance.*function)(args...);
        };
    }
    
    //Pointer to instance
    template<typename F, typename I>
    std::function<void(Args...)> objectBind(F&& function, I* instance) const {
        return objectBind(function, *instance);
    }
    
    std::map<uint32_t, std::function<void(Args...)>> *getSlotMap(const ExecutorScheme &scheme) const{
        switch(scheme){
            case (ExecutorScheme::ASYNCHRONOUS):
                return &asynchronousSlots;
            case (ExecutorScheme::STRAND):
                return &strandSlots;
            case (ExecutorScheme::THREAD_POOLED):
                return &threadPooledSlots;
            default:
            case (ExecutorScheme::SYNCHRONOUS):
                return &synchronousSlots;
        }
    }
    
    std::map<uint32_t, std::function<void(Args...)>> *findSlotMapWithId(const uint32_t &id) const{
        if (synchronousSlots.count(id))
            return &synchronousSlots;
        if (asynchronousSlots.count(id))
            return &asynchronousSlots;
        if (strandSlots.count(id))
            return &strandSlots;
        if (threadPooledSlots.count(id))
            return &threadPooledSlots;
        return nullptr;
    }
        
    void queueListener(const uint32_t &id) const{
        auto &q = strandQueues[id];
        std::function<void()> func = [](){};
        auto maxWait = BSignals::details::WheeledThreadPool::getMaxWait();
        std::chrono::duration<double> waitTime = std::chrono::nanoseconds(1);
        while (func){
            if (q.dequeue(func)){
                if (func) func();
                waitTime = std::chrono::nanoseconds(1);
            }
            else{
                std::this_thread::sleep_for(waitTime);
                waitTime*=2;
            }
            if (waitTime > maxWait){
                q.blockingDequeue(func);
                if (func) func();
                waitTime = std::chrono::nanoseconds(1);
            }
        }
    }
    
    //Shared mutex for thread safety
    //Emit acquires shared lock, connect/disconnect acquires unique lock
    mutable std::shared_timed_mutex signalLock;
    
    //Atomically incremented slotId
    mutable std::atomic<uint32_t> currentId {0};
    
    //Async Emit Semaphore
    mutable BSignals::details::Semaphore sem {1024};
    
    //EmissionGuard determines if it is necessary to guard emission with a shared mutex
    //This is only required if connection/disconnection could be interleaved with emission
    const bool enableEmissionGuard {false};
    
    //Strand Queues and Threads
    mutable std::map<uint32_t, BSignals::details::MPSCQueue<std::function<void()>>> strandQueues;
    mutable std::map<uint32_t, std::thread> strandThreads;
    
    //Slot Maps
    mutable std::map<uint32_t, std::function<void(Args...)>> synchronousSlots;
    mutable std::map<uint32_t, std::function<void(Args...)>> asynchronousSlots;
    mutable std::map<uint32_t, std::function<void(Args...)>> strandSlots;
    mutable std::map<uint32_t, std::function<void(Args...)>> threadPooledSlots;

};

}}

#endif /* SIGNALIMPL_HPP */
