#pragma once

#include <thread>
#include <future>
#include <condition_variable>
#include <mutex>
#include <list>
#include <memory>
#include <cassert>
#include <algorithm>

namespace threadpool {

  template <class Result> class TaskHandler;
  class Threadpool {
  public:
    // Defines how the tasks will be ordered by default
    enum TaskOrdering { 
      FIFO, // First In First OUT
      LIFO  // Last In First OUT
    };

    // Priority with which a task will be queued
    enum TaskPriority { 
      MAX,      // The task will be the next one to execute
      DEFAULT,  // FIFO or LIFO
      MIN       // The task will be put past all stored tasks
    };
    

    /**
     * @brief Create a threadpool holding a certain number of threads
     * 
     * @param int number of workers
     * @param ordering default ordering function for tasks
     *                 FIFO = First In First Out
     *                 LIFO = Last In First Out
     * 
     */
    explicit Threadpool(unsigned int nWorkers, TaskOrdering ordering = FIFO) :
      mOrdering(ordering),
      mTasksInProgress(0),
      mThreadsToKill(0),
      mNWorkers(0),
      mPoolDestroyed(false)
    {
      for (auto i = 0u; i < nWorkers; ++i) {
        mWorkers.emplace_back(&Threadpool::workerFunction, this);
        ++mNWorkers;
      }
    }

    /**
     * @brief Join all worker threads, leaving all remaining tasks undone
     */
    ~Threadpool() {
      {
        std::lock_guard<std::mutex> lock(mTasksMutex);
        mPoolDestroyed = true;
        mThreadsToKill = 0; // cancel possible thread suicides
      }
      mTasksCond.notify_all();
      for (auto it = std::begin(mWorkers); it != std::end(mWorkers); ++it) {
        it->join();
      }
    }

    /**
     * @brief Queue a task and return a handle to it
     * 
     * @param fn Callable
     * @param args Arguments for fn
     * @tparam Priority
     * @return Handle to the task
     */
    template <TaskPriority Priority = DEFAULT, class FN, class... Args>
    auto queueAndHandleTask(FN&& fn, Args&&... args)
      -> TaskHandler<typename std::result_of<FN(Args...)>::type>
    {
      using return_type = typename std::result_of<FN(Args...)>::type;
      auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<FN>(fn), std::forward<Args>(args)...)
      );

      queueTask<Priority>([task](){ (*task)(); });

      return TaskHandler<return_type>(task);
    }

    /**
     * @brief Queue a task
     * 
     * @param fn Callable
     * @param args Arguments for fn
     * @tparam Priority 
     */
    template <TaskPriority Priority = DEFAULT, class FN, class... Args>
    void queueTask(FN&& fn, Args&&... args) {
      {
        std::lock_guard<std::mutex> lock(mTasksMutex);
        auto insertPosition = getInsertPosition(Priority, mOrdering);
        mTasks.emplace(insertPosition, std::bind(std::forward<FN>(fn), std::forward<Args>(args)...));
      }
      mTasksCond.notify_one();
    }

    /**
     * @brief Changes the default task ordering for the threadpool
     * @param newOrdering 
     */
    void setDefaultOrdering(TaskOrdering newOrdering) { mOrdering = newOrdering; }

    /**
     * @brief Change the number of threads managed by the threadpool
     * @param int new thread number
     */
    void resize(unsigned int newWorkers) {
      int workerDiff = newWorkers - mNWorkers;

      if (workerDiff > 0) {
        std::lock_guard<std::mutex> lock(mTasksMutex);
        for (int i = 0; i < workerDiff; ++i) {
          mWorkers.emplace_back(&Threadpool::workerFunction, this);
          ++mNWorkers; // exception safety
        }
      }
      else if (workerDiff < 0) {
        {
          mNWorkers = newWorkers;
          std::lock_guard<std::mutex> lock(mTasksMutex);
          mThreadsToKill += -workerDiff; // in case mThreadsToKill > 0
        }
        // 'mThreadsToKill' not used to prevent data races when threads
        // while we are still in this loop
        for (int i = 0; i < -workerDiff; ++i) {
          mTasksCond.notify_one();
        }
      }
    }

    /**
     * @brief Blocks calling thread until there are no tasks left to execute
     */
    void waitForTasks() {
      std::unique_lock<std::mutex> lock(mWaitMutex);
      if (!mTasks.empty() || mTasksInProgress > 0) {
        mWaitCond.wait(lock, [this] { return mTasks.empty() && mTasksInProgress == 0; });
      }
    }

    /**
    * @brief Returns the current number of worker threads in the pool
    */
    unsigned int workerCount() const {
      return mNWorkers;
    }

  private:
    Threadpool(const Threadpool&) = delete;

    enum WorkerAction {
      WAIT,
      WORK,
      EXIT,
      SUICIDE
    };

    WorkerAction getNextAction() {
      if (mPoolDestroyed) return EXIT;
      if (mThreadsToKill > 0) return SUICIDE;
      if (!mTasks.empty()) return WORK;
      return WAIT;
    }

    std::list<std::function<void()>>::const_iterator
    getInsertPosition(TaskPriority priority, TaskOrdering ordering) {
      if (priority == MAX) return mTasks.cbegin();
      if (priority == MIN) return mTasks.cend();
      return (ordering == LIFO) ? mTasks.cbegin() : mTasks.cend();
    }

    void workerFunction() {
      WorkerAction nextAction;
      decltype(mTasks)::value_type nextTask;
      while (true) {
        {
          std::unique_lock<std::mutex> lock(mTasksMutex);
          nextAction = getNextAction();
          if (nextAction == WAIT) {
            mTasksCond.wait(lock, [this, &nextAction] {return (nextAction = getNextAction()) != WAIT; });
          }
          if (nextAction == EXIT) {
            break;
          }
          else if (nextAction == SUICIDE) {
            auto threadIt = std::find_if(std::begin(mWorkers), std::end(mWorkers), [](const std::thread& thread) {
              return thread.get_id() == std::this_thread::get_id();
            });
            assert(mThreadsToKill > 0 && !mWorkers.empty() && threadIt != std::end(mWorkers));
            threadIt->detach();
            mWorkers.erase(threadIt);
            --mThreadsToKill;
            break;
          }
          else if (nextAction == WORK) {
            // Lock in case some thread is waiting for all tasks to complete
            std::lock_guard<std::mutex> lock(mWaitMutex);
            nextTask = std::move(mTasks.front());
            mTasks.pop_front();
            ++mTasksInProgress;
          }
        }
        assert(nextAction == WORK);
        nextTask();
        {
          std::lock_guard<std::mutex> lock(mWaitMutex);
          --mTasksInProgress;
        }
        mWaitCond.notify_all();
      }
    }

    // list used to prevent iterator invalidation
    std::list<std::thread> mWorkers;
    std::list<std::function<void()>> mTasks;
    TaskOrdering mOrdering;

    // protect and sync members that determine next thread action:
    // - mTasks
    // - mThreadsToKill
    // - mPoolDestroyed
    std::condition_variable mTasksCond;
    std::mutex mTasksMutex;

    // allow a thread to wait for remaining tasks to complete
    // protect mTasksInProgress
    std::condition_variable mWaitCond;
    std::mutex mWaitMutex;

    unsigned int mTasksInProgress;
    unsigned int mThreadsToKill; // Threads to kill due to a resize
    unsigned int mNWorkers;      // Workers after all threads to kill are dead

    bool mPoolDestroyed;
  };

  /**
  * @brief Handles a task state and allows to get the task result
  *
  * @tparam Result type of the task result
  */
  template <class Result>
  class TaskHandler {
    // Users are not allow to construct new tasks handlers
    template <Threadpool::TaskPriority Priority, class FN, class... Args>
    friend TaskHandler<typename std::result_of<FN(Args...)>::type>
    Threadpool::queueAndHandleTask(FN&&, Args&&...);

  public:
    TaskHandler(TaskHandler&& other) :
      mTaskHandle(std::move(other.mTaskHandle)),
      mResult(std::move(other.mResult))
    {}

    TaskHandler& operator=(TaskHandler&& other) {
      mTaskHandle = std::move(other.mTaskHandle);
      mResult = std::move(other.mResult);
      return *this;
    }

    /**
    * @brief Returns wheter tha task has completed
    * @return bool
    */
    bool finished() const {
      return mResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    /**
    * @brief Get the task result/propagate the exception thrown by the task
    * @details Blocks the calling thread until the result is ready
    *          Users are responsible for calling this method only once
    * @return The value the task returned
    */
    Result getResult() { return mResult.get(); }

    /**
    * @brief Block the calling thread until the result is ready
    */
    void wait() const { mResult.wait(); }

  private:
    explicit TaskHandler(const std::shared_ptr<std::packaged_task<Result()>>& task) :
      mTaskHandle(task),
      mResult(task->get_future())
    {}

    TaskHandler(const TaskHandler&) = delete;

    std::shared_ptr <std::packaged_task<Result()>> mTaskHandle;
    std::future<Result> mResult;
  };
}