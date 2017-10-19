// Threadpool.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "Threadpool.h"
#include <iostream>
#include <string>
#include <cassert>

void printMessage(const std::string& msg) {
  std::this_thread::sleep_for(std::chrono::seconds{ 2 });
  std::cout << "Thread " << std::this_thread::get_id() << " says: " << msg << "\n";
}

struct Printer {
  std::string msg;
  void operator()() const { printMessage(msg); }
};

struct Message {
  std::string msg;
  void print() const { printMessage(msg); }
};

int waitAndAdd(int lhs, int rhs) {
  std::this_thread::sleep_for(std::chrono::seconds{ 2 });
  return lhs + rhs;
}

int main()
{
  threadpool::Threadpool threadpool{ 4 };

  // Free functions
  threadpool.queueTask(&printMessage, "Hi from free function");

  // Lambdas
  threadpool.queueTask([](const std::string& msg) {
    printMessage(msg);
  }, "Hi from lambda");

  // Functors
  threadpool.queueTask(Printer{ "Hi from functor" });

  // Member methods
  Message m{ "Hi from member method" };
  // use std::ref and std::cref to pass-by-reference
  threadpool.queueTask(&Message::print, std::cref(m)); 

  threadpool.waitForTasks(); // Wait for all tasks to complete

  std::this_thread::sleep_for(std::chrono::seconds{ 3 });

  // Handle individual tasks
  auto loadHandler = threadpool.queueAndHandleTask([]() {
    std::cout << "Loading system ...\n";
    std::this_thread::sleep_for(std::chrono::seconds{ 4 });
  });

  std::this_thread::sleep_for(std::chrono::seconds{ 1 });
  std::cout << "Is the system loaded? " << (loadHandler.finished() ? "Yes" : "No") << "\n";
  std::cout << "Waiting for system to load ...\n";
  loadHandler.wait();
  std::cout << "System loaded!\n";

  // Get results from tasks
  auto addHandler = threadpool.queueAndHandleTask(&waitAndAdd, 2, 2);
  // Wait for the result
  int result = addHandler.getResult();
  assert(result == 4);

  auto addThreeAnd = std::bind(&waitAndAdd, 3, std::placeholders::_1);
  auto addBindedHandler = threadpool.queueAndHandleTask(addThreeAnd, 5);
  addBindedHandler.wait();
  assert(addBindedHandler.getResult() == 8);

  return 0;
}

