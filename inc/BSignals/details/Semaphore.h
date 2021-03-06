/* 
 * File:   Semaphore.h
 * Author: Barath Kannan
 *
 * Created on 30 May 2016, 6:49 PM
 */

#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <atomic>
#include <mutex>
#include <condition_variable>

namespace BSignals{ namespace details{
class Semaphore{
public:
    Semaphore(uint32_t size);
    ~Semaphore();
    
    void acquire();
    void release();
private:
    std::mutex semMutex;
    std::condition_variable semCV;
    uint32_t semCounter;
};
}}
#endif /* SEMAPHORE_H */

