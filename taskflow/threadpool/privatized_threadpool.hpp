// 2018/09/12 - created by Tsung-Wei Huang and Chun-Xun Lin
//
// Speculative threadpool is similar to proactive threadpool except
// each thread will speculatively move a new task to its local worker
// data structure to reduce extract hit to the task queue.
// This can save time from locking the mutex during dynamic tasking.

#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <stdexcept>
#include <condition_variable>
#include <memory>
#include <future>
#include <unordered_set>
#include <unordered_map>


namespace privatized_threadpool {

template <typename T, unsigned N>
class RunQueue {

  static_assert((N & (N - 1)) == 0, "N must be power of two");
  static_assert(N > 2, "N must be larger than two");
  
  constexpr static unsigned IDX_MASK = N - 1;
  constexpr static unsigned POS_MASK = (N << 1) - 1;

  struct Entry {
    std::atomic<uint8_t> state;
    T w;
  };

  enum : uint8_t {
    EMPTY,
    BUSY,
    READY
  };

  public:

    RunQueue();

    bool push_front(T&&);
    bool push_front(T&);
    bool pop_front(T&);

    bool push_back(T&&);
    bool push_back(T&);
    bool pop_back(T&);

    bool empty() const;

  private:

    std::mutex _mutex;

    std::atomic<unsigned> _front;
    std::atomic<unsigned> _back;

    Entry _array[N];
};

// Constructor    
template <typename T, unsigned N>
RunQueue<T, N>::RunQueue() : _front(0), _back(0) {
  for(unsigned i=0; i<N; ++i) {
    _array[i].state.store(EMPTY, std::memory_order_relaxed);
  }
}

// insert item w to the beginning of the queue and return true if inserted 
// or false otherwise.
// this function can only be called by the owner thread.
template <typename T, unsigned N>
bool RunQueue<T, N>::push_front(T& w) {

  auto front = _front.load(std::memory_order_relaxed);
  auto& item = _array[front & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);
  
  if(state != EMPTY || 
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }

  _front.store((front + 1) & POS_MASK, std::memory_order_relaxed);
  item.w = std::move(w);
  item.state.store(READY, std::memory_order_release);

  return true;
}

template <typename T, unsigned N>
bool RunQueue<T, N>::push_front(T&& w) {
  return push_front(w);
}


// pop the first item out of the queue and store it to w
template <typename T, unsigned N>
bool RunQueue<T, N>::pop_front(T& w) {

  if(empty()) {
    return false;
  }

  auto front = _front.load(std::memory_order_relaxed);
  auto& item = _array[(front - 1) & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if(state != READY || 
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }
  
  _front.store((front - 1) & POS_MASK, std::memory_order_relaxed);
  w = std::move(item.w); 
  item.state.store(EMPTY, std::memory_order_release);

  return true;
}

// add an item at the end of the queue
template <typename T, unsigned N>
bool RunQueue<T, N>::push_back(T& w) {

  std::scoped_lock lock(_mutex);

  auto back  = _back.load(std::memory_order_relaxed);
  auto& item = _array[(back - 1) & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if(state != EMPTY ||
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }
  
  _back.store((back - 1) & POS_MASK, std::memory_order_relaxed);
  item.w = std::move(w);
  item.state.store(READY, std::memory_order_release);

  return true;
}

// add an item at the end of the queue
template <typename T, unsigned N>
bool RunQueue<T, N>::push_back(T&& w) {
  return push_back(w);
}


// pop_back removes and returns the last elements in the queue.
// Can fail spuriously.
template <typename T, unsigned N>
bool RunQueue<T, N>::pop_back(T& w) {

  if(empty()) {
    return false;
  }

  std::unique_lock lock(_mutex, std::try_to_lock);

  if (!lock) {
    return false;
  }

  auto back  = _back.load(std::memory_order_relaxed);
  auto& item = _array[back & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if (state != READY ||
     !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }

  w = std::move(item.w);
  _back.store((back + 1) & POS_MASK, std::memory_order_relaxed);
  item.state.store(EMPTY, std::memory_order_release);

  return true;
}

// EMPTY tests whether container is empty.
// Can be called by any thread at any time.
template <typename T, unsigned N>
bool RunQueue<T, N>::empty() const { 
  return _front.load(std::memory_order_relaxed) ==
         _back.load(std::memory_order_relaxed);
}


template <typename T>
struct MoC {

  MoC(T&& rhs): object(std::move(rhs)) {}
  MoC(const MoC& other) : object(std::move(other.object)) {}

  T& get() {return object; }
  
  mutable T object;
};


template < template<typename...> class Func >
class BasicPrivatizedThreadpool {

  using TaskType = Func<void()>;
  using WorkQueue = RunQueue<TaskType, 1024>;

  struct Worker{
    std::condition_variable cv;
    WorkQueue queue;
  };

  public:

    BasicPrivatizedThreadpool(unsigned);
    ~BasicPrivatizedThreadpool();

    size_t num_tasks() const;
    size_t num_workers() const;

    bool is_owner() const;

    void shutdown();
    void spawn(unsigned);
    void wait_for_all();

    template <typename C>
    void silent_async(C&&);

    template <typename C>
    auto async(C&&);

  private:

    mutable std::mutex _mutex;

    std::condition_variable _empty_cv;

    std::deque<TaskType> _task_queue;
    std::vector<std::thread> _threads;
    std::atomic<size_t> _idle_workers {0}; 
    std::unordered_map<std::thread::id, size_t> _worker_map;    
    
    const std::thread::id _owner {std::this_thread::get_id()};

    bool _exiting      {false};
    bool _wait_for_all {false};

    std::vector<std::unique_ptr<Worker>> _works;
    std::atomic<size_t> _next_queue {0};

    size_t _nonempty_queue() const;

    bool _sync {false};

    std::vector<size_t> _coprimes;
    void _xorshift32(uint32_t&);
    bool _steal(TaskType&, uint32_t&);

};  // class BasicPrivatizedThreadpool. --------------------------------------


// Function: _nonempty_queue
template < template<typename...> class Func >
size_t BasicPrivatizedThreadpool<Func>::_nonempty_queue() const {
  for(size_t i=0;i <_works.size(); ++i){
    if(!_works[i]->queue.empty()){
      return i;
    }
  }
  return _works.size();
}

// Function: _xorshift32
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::_xorshift32(uint32_t& x){
  // x must be non zero: https://en.wikipedia.org/wiki/Xorshift
  // Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" 
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
}

// Function: _steal
template < template<typename...> class Func >
bool BasicPrivatizedThreadpool<Func>::_steal(TaskType& w, uint32_t& dice){
  _xorshift32(dice);
  const auto inc = _coprimes[dice % _coprimes.size()];
  const auto queue_num = num_workers();
  auto victim = dice % queue_num;
  for(size_t i=0; i<queue_num; i++){
    if(_works[victim]->queue.pop_back(w)){
      return true;
    }
    victim += inc;
    if(victim >= queue_num){
      victim -= queue_num;
    }
  }
  return false;
}



// Constructor
template < template<typename...> class Func >
BasicPrivatizedThreadpool<Func>::BasicPrivatizedThreadpool(unsigned N){
  spawn(N);
}

// Destructor
template < template<typename...> class Func >
BasicPrivatizedThreadpool<Func>::~BasicPrivatizedThreadpool(){
  shutdown();
}

// Function: is_owner
template < template<typename...> class Func >
bool BasicPrivatizedThreadpool<Func>::is_owner() const {
  return std::this_thread::get_id() == _owner;
}

// Function: num_tasks
template < template<typename...> class Func >
size_t BasicPrivatizedThreadpool<Func>::num_tasks() const { 
  return _task_queue.size(); 
}

// Function: num_workers
template < template<typename...> class Func >
size_t BasicPrivatizedThreadpool<Func>::num_workers() const { 
  return _threads.size();  
}

// Function: shutdown
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::shutdown(){

  if(!is_owner()){
    throw std::runtime_error("Worker thread cannot shut down the pool");
  }

  if(_threads.empty()) {
    return;
  }

  { 
    std::unique_lock<std::mutex> lock(_mutex);
    _wait_for_all = true;

    // Wake up all workers in case they are already idle
    for(const auto& w : _works){
      w->cv.notify_one();
    }

    //while(_idle_workers != num_workers()) {
    while(!_sync){
      _empty_cv.wait(lock);
    }
    _sync = false;
    _exiting = true;

    for(auto& w : _works){
      w->queue.push_back([](){});
      w->cv.notify_one();
    }
  }

  for(auto& t : _threads){
    t.join();
  } 
  _threads.clear();  

  _works.clear();
  _worker_map.clear();
  
  _wait_for_all = false;
  _exiting = false;
  
  // task queue might have tasks added by threads outside this pool...
  //while(not _task_queue.empty()) {
  //  std::invoke(_task_queue.front());
  //  _task_queue.pop_front();
  //}
}

// Function: spawn 
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::spawn(unsigned N) {

  if(! is_owner()){
    throw std::runtime_error("Worker thread cannot spawn threads");
  }

  // Wait untill all workers become idle if any
  if(!_threads.empty()){
    wait_for_all();
  }

  const size_t sz = _threads.size();

  // Lock to synchronize all workers before creating _worker_maps
  std::scoped_lock<std::mutex> lock(_mutex);

  _coprimes.clear();
  for(size_t i=1; i<=sz+N; i++){
    if(std::gcd(i, sz+N) == 1){
      _coprimes.push_back(i);
    }
  }

  for(size_t i=0; i<N; ++i){
    _works.push_back(std::make_unique<Worker>());
  }

  for(size_t i=0; i<N; ++i){
    _threads.emplace_back([this, i=i+sz]() -> void {

       TaskType t {nullptr};
       Worker& w = *(_works[i]);
       uint32_t dice = i+1;
       std::unique_lock<std::mutex> lock(_mutex);

       while(!_exiting){
         if(!w.queue.pop_front(t)){
           if(_steal(t, dice)){}
           else if(!_task_queue.empty()) {
           //if(!_task_queue.empty()){
             t = std::move(_task_queue.front());
             _task_queue.pop_front();
           } 
           else {
             while(!w.queue.pop_front(t) && _task_queue.empty()){
               if(++_idle_workers == num_workers() && _wait_for_all){
                 // Last active thread checks if all queues are empty
                 if(auto ret = _nonempty_queue(); ret == num_workers()){
                   _sync = true;
                   _empty_cv.notify_one();
                 }
                 else{
                   if(ret == i){
                     -- _idle_workers;
                     continue;
                   }
                   _works[ret]->cv.notify_one();
                 }
               } 
               w.cv.wait(lock);
               -- _idle_workers;
             }
           }
         } // End of first if

         if(t){
           _mutex.unlock();
           t();
           t = nullptr;
           _mutex.lock();
         }
       } // End of while ------------------------------------------------------
    });     

    _worker_map.insert({_threads.back().get_id(), i+sz});
  } // End of For ---------------------------------------------------------------------------------

}


// Function: async
template < template<typename...> class Func >
template <typename C>
auto BasicPrivatizedThreadpool<Func>::async(C&& c){

  using R = std::invoke_result_t<C>;

  std::promise<R> p;
  auto fu = p.get_future();
  
  // master thread
  if(num_workers() == 0){
    if constexpr(std::is_same_v<void, R>){
      c();
      p.set_value();
    }
    else{
      p.set_value(c());
    } 
  }
  // have worker(s)
  else{
    if constexpr(std::is_same_v<void, R>){
      silent_async( 
        [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
          c();
          p.get().set_value(); 
        }
      );
      //std::scoped_lock<std::mutex> lock(_mutex);     
      //if(_idle_workers.empty()){
      //  _task_queue.emplace_back(
      //    [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
      //      c();
      //      p.get().set_value(); 
      //    }
      //  );
      //}
      //// Got an idle work
      //else{
      //  Worker* w = _idle_workers.back();
      //  _idle_workers.pop_back();
      //  w->ready = true;
      //  w->task = [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
      //    c();
      //    p.get().set_value(); 
      //  };
      //  w->cv.notify_one(); 
      //}
    }
    else{
      silent_async( 
        [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
          p.get().set_value(c()); 
        }
      );
      //std::scoped_lock<std::mutex> lock(_mutex);     
      //if(_idle_workers.empty()){
      //  _task_queue.emplace_back(
      //    [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
      //      p.get().set_value(c());
      //    }
      //  );
      //}
      //else{
      //  Worker* w = _idle_workers.back();
      //  _idle_workers.pop_back();
      //  w->ready = true;
      //  w->task = [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
      //    p.get().set_value(c()); 
      //  };
      //  w->cv.notify_one(); 
      //}
    }
  }
  return fu;
}

template < template<typename...> class Func >
template <typename C>
void BasicPrivatizedThreadpool<Func>::silent_async(C&& c){

  TaskType t {std::forward<C>(c)};

  //no worker thread available
  if(num_workers() == 0){
    t();
    return;
  }

  if(std::this_thread::get_id() != _owner){
    auto tid = std::this_thread::get_id();
    if(_worker_map.find(tid) != _worker_map.end()){
      if(!_works[_worker_map.at(tid)]->queue.push_front(t)){
        std::scoped_lock<std::mutex> lock(_mutex);       
        _task_queue.push_back(std::move(t));
      }
      return ;
    }
  }

  auto id = (++_next_queue)%_works.size();
  if(!_works[id]->queue.push_back(t)){
    std::scoped_lock<std::mutex> lock(_mutex);
    _task_queue.push_back(std::move(t));
  }

  // Make sure at least one worker will handle the task
  _works[id]->cv.notify_one();
}


// Function: wait_for_all
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::wait_for_all() {

  if(!is_owner()){
    throw std::runtime_error("Worker thread cannot wait for all");
  }

  if(num_workers() == 0) return ;

  std::unique_lock<std::mutex> lock(_mutex);
  _wait_for_all = true;
  // Wake up all workers in case they are already idle
  for(const auto& w : _works){
    w->cv.notify_one();
  }

  while(!_sync){
    _empty_cv.wait(lock);
  }

  _sync = false;
  _wait_for_all = false;
}



};  // namespace privatized_threadpool. --------------------------------------




