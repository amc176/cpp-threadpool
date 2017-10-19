# cpp-threadpool
A solid and flexible threadpool using standard c++11 features (header-only).

## Basic usage
```C++
int main() {
  // Create a threadpool with 5 worker threads
  Threadpool threadpool{ 5 };
  
  // Give them some work to do
  threadpool.queueTask([](){
    std::cout<<"Hi from a worker thread\n";
  });
  
  Message m{ "Hello world" };
  threadpool.queueTask(&Printer::printMessage, std::cref(m));
  ...
```

### What can the threadpool receive?
- Free functions, lambdas
- Static/member methods, functors
- `std::function`, return values from `std::bind`, etc

Note that callables are binded with their arguments using `std::bind`, so to pass-by-reference `std::ref`/`std::cref` should be used.

In the file [main.cpp](Threadpool/main.cpp) you can find some examples.

## Handling task result
Users can handle task state and result through handlers returned by `Threapool::queueAndHandleTask`:
```C++
auto handler = threadpool.queueAndHandleTask(&getMeaningOfLife);

std::this_thread::sleep_for(std::chrono::seconds(3));
// Is the task completed?
if (handler.finished()) {
  std::cout << "The meaning of life is " << handler.getResult() << "\n";
}
else {
  // If the result is not ready, block this thread until it is
  int meaningOfLife = handler.getResult();
}
```
**Important:** `TaskHandler::getResult` must be called **only once**. Not doing so might throw or cause UB.

## Task ordering
Tasks are ordered following the methods FIFO or LIFO.
Users can specify the default task ordering via the Threadpool constructor:
```C++
class Threadpool {
  public:
    // Defines how the tasks will be ordered by default
    enum TaskOrdering { 
      FIFO, // First In First OUT
      LIFO  // Last In First OUT
    };
    
    explicit Threadpool(unsigned int nWorkers, TaskOrdering ordering = FIFO) {
  ...
  
Threadpool threadpool{ 5, Threadpool::LIFO };
```

As there can be tasks more important than others, a template argument with the **task priority** can be specified to `Threadpool::queueTask`and `Threadpool::queueAndHandleTask`:
```C++
    enum TaskPriority { 
      MAX,      // The task will be the next one to execute
      DEFAULT,  // FIFO or LIFO
      MIN       // The task will be put past all stored tasks
    };
  ...
  
threadpool.queueTask(&normalTask); // Default ordering
threadpool.queueTask<Threadpool::MIN>(&notImportantTask);
threadpool.queueAndHandleTask<Threadpool::MAX>(&veryImportantTask);
```
## Thread management
The threadpool can be easily resized in runtime by calling `Threadpool::resize(unsigned int newThreadAmount)`
```C++
  ...
  threadpool.resize(8);
  ...
```

## Threadpool destruction
When the threadpool is destroyed, remaining tasks are left undone.
To ensure all tasks are done before destroying the threadpool, users need to call `Threadpool::waitForTasks`.
```C++
int main() {
  Threadpool threadpool{ 1 };
  for (unsigned int i = 0; i < 10; ++i) {
    threadpool.queueTask([]() {std::this_thread::sleep_for(std::chrono::seconds(6)); });
  }
  threadpool.waitForTasks();
  // 1 minute later ...
  ...
```

## Exception safety
Threadpool constructor and its member `resize` offer *basic exception guarantee*.
If they throw (probably because the system can't allocate new threads) the threadpool will be in a valid state, holding as many threads as it had prior to the exception throw.

If an *unhandled* task throws, `std::terminate` is called.

If a *handled* task throws, its exception will be propagated to the thread that calls `TaskHandler::getResult`.
