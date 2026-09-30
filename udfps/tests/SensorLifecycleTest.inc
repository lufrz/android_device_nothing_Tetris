// SPDX-License-Identifier: Apache-2.0
// Runs the production Sensor.cpp with only sysfs file descriptors simulated.
#include "Sensor.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <string>

using namespace android::hardware::sensors::V2_1::subhal::implementation;
using namespace std::chrono_literals;

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_pipe2(int*, int);
extern "C" int __real_close(int);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" off_t __real_lseek(int, off_t, int);
extern "C" int __real_poll(struct pollfd*, nfds_t, int);

namespace {
struct Node { int writer; bool udfps; };
std::mutex ioMutex;
std::map<int, Node> nodes;
std::set<int> ownedFds;
std::atomic_bool missingNodes{false};
std::atomic_bool failPipe{false};
std::atomic_bool holdEvent{false};
std::mutex gateMutex;
std::condition_variable gateCV;
bool eventHeld = false;
bool releaseEvent = false;

class Callback final : public ISensorsEventCallback {
 public:
  void postEvents(const std::vector<Event>& posted, bool wakeup) override {
    std::lock_guard lock(mutex);
    assert(wakeup);
    events.insert(events.end(), posted.begin(), posted.end());
    changed.notify_all();
  }
  size_t count() { std::lock_guard lock(mutex); return events.size(); }
  Event waitOne() {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 1s, [&] { return !events.empty(); }));
    assert(events.size() == 1);
    return events.front();
  }
 private:
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Event> events;
};

void emit(bool udfps) {
  std::lock_guard lock(ioMutex);
  for (const auto& [fd, node] : nodes) {
    (void)fd;
    if (node.udfps == udfps) {
      const char byte = '1';
      assert(write(node.writer, &byte, 1) == 1);
      return;
    }
  }
  assert(false && "sensor node not opened");
}

void checkNoFds() {
  std::lock_guard lock(ioMutex);
  assert(nodes.empty() && ownedFds.empty());
}

template<class T> void exercise(bool udfps) {
  // No worker may observe a half-constructed object, and NORMAL must work without
  // an explicit setOperationMode() call from the HAL client.
  Callback callback;
  auto sensor = std::make_unique<T>(7, &callback);
  sensor->activate(true);
  emit(udfps);
  std::this_thread::sleep_for(20ms);
  assert(callback.count() == 0);
  sensor->start();
  sensor->start(); // harmless duplicate, not a second worker
  auto event = callback.waitOne();
  assert(event.sensorHandle == 7);
  if (udfps) assert(event.u.data[0] == 540 && event.u.data[1] == 2109);
  for (int slot = udfps ? 2 : 0; slot < 16; ++slot) assert(event.u.data[slot] == 0);
  emit(udfps);
  std::this_thread::sleep_for(20ms);
  assert(callback.count() == 1); // one-shot remains disabled
  sensor.reset();
  checkNoFds();

  // Fill the interrupt pipe while no reader exists. Old blocking writes hang
  // under mRunMutex here; the worker could not drain it after acquiring that lock.
  Callback fullCallback;
  auto full = std::make_unique<T>(8, &fullCallback);
  for (int i = 0; i < 20000; ++i) {
    full->setOperationMode(i % 2 ? OperationMode::NORMAL : OperationMode::DATA_INJECTION);
  }
  full->start();
  full->activate(true);
  emit(udfps);
  fullCallback.waitOne();
  full.reset();
  checkNoFds();

  // Destruction must wake a real blocking poll and join before closing its fds.
  for (int i = 0; i < 30; ++i) {
    Callback quiet;
    auto polling = std::make_unique<T>(i, &quiet);
    polling->start();
    polling->activate(true);
    std::this_thread::sleep_for(1ms);
    polling.reset();
    assert(quiet.count() == 0);
    checkNoFds();
  }

  // Force the disable/poll race: hold an already-delivered sysfs event before
  // production run() takes its mutex again, then deactivate the sensor.
  Callback cancelled;
  auto raced = std::make_unique<T>(9, &cancelled);
  {
    std::lock_guard lock(gateMutex);
    eventHeld = false;
    releaseEvent = false;
  }
  holdEvent = true;
  raced->start();
  raced->activate(true);
  emit(udfps);
  {
    std::unique_lock lock(gateMutex);
    assert(gateCV.wait_for(lock, 1s, [] { return eventHeld; }));
  }
  raced->activate(false);
  {
    std::lock_guard lock(gateMutex);
    releaseEvent = true;
  }
  gateCV.notify_all();
  std::this_thread::sleep_for(20ms);
  assert(cancelled.count() == 0);
  raced.reset();
  checkNoFds();

  // Failed pipe creation leaves an unstarted, safely destructible object.
  failPipe = true;
  Callback failed;
  auto unavailable = std::make_unique<T>(10, &failed);
  unavailable->start();
  unavailable.reset();
  checkNoFds();

  // The sysfs producer may appear later. Closing during retry must not wait for
  // the retry deadline or leak descriptors.
  missingNodes = true;
  auto retry = std::make_unique<T>(11, &failed);
  retry->start();
  retry->activate(true);
  std::this_thread::sleep_for(5ms);
  auto before = std::chrono::steady_clock::now();
  retry.reset();
  assert(std::chrono::steady_clock::now() - before < 200ms);
  missingNodes = false;
  checkNoFds();
}
} // namespace

extern "C" int __wrap_pipe2(int* fds, int flags) {
  if (failPipe.exchange(false)) { errno = EMFILE; return -1; }
  int rc = __real_pipe2(fds, flags);
  if (rc == 0) {
    assert(flags & O_NONBLOCK);
    assert(flags & O_CLOEXEC);
    std::lock_guard lock(ioMutex);
    ownedFds.insert(fds[0]); ownedFds.insert(fds[1]);
  }
  return rc;
}
extern "C" int __wrap_open(const char* path, int flags, ...) {
  const bool udfps = std::strstr(path, "fts_gesture_fod_pressed") != nullptr;
  const bool single = std::strstr(path, "fts_gesture_single_tap_pressed") != nullptr;
  if (!udfps && !single) return __real_open(path, flags);
  if (missingNodes) { errno = ENOENT; return -1; }
  int fds[2];
  assert(__real_pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
  std::lock_guard lock(ioMutex);
  nodes.emplace(fds[0], Node{fds[1], udfps});
  ownedFds.insert(fds[0]); ownedFds.insert(fds[1]);
  return fds[0];
}
extern "C" int __wrap_close(int fd) {
  std::lock_guard lock(ioMutex);
  auto node = nodes.find(fd);
  if (node != nodes.end()) {
    __real_close(node->second.writer);
    ownedFds.erase(node->second.writer);
    nodes.erase(node);
  }
  ownedFds.erase(fd);
  return __real_close(fd);
}
extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence) {
  std::lock_guard lock(ioMutex);
  return nodes.count(fd) ? 0 : __real_lseek(fd, offset, whence);
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
  std::lock_guard lock(ioMutex);
  auto node = nodes.find(fd);
  if (node == nodes.end()) return __real_read(fd, buffer, size);
  char byte;
  if (__real_read(fd, &byte, 1) != 1) return -1;
  const char* data = node->second.udfps ? "540,2109,1" : "1";
  size_t length = std::min(size, std::strlen(data));
  std::memcpy(buffer, data, length);
  return static_cast<ssize_t>(length);
}
extern "C" int __wrap_poll(struct pollfd* fds, nfds_t count, int timeout) {
  std::vector<struct pollfd> fixture(fds, fds + count);
  std::set<int> sysfs;
  {
    std::lock_guard lock(ioMutex);
    for (auto& fd : fixture) if (nodes.count(fd.fd)) { fd.events = POLLIN; sysfs.insert(fd.fd); }
  }
  int rc = __real_poll(fixture.data(), count, timeout);
  bool hasEvent = false;
  for (nfds_t i = 0; i < count; ++i) {
    fds[i].revents = fixture[i].revents;
    if (sysfs.count(fds[i].fd) && (fds[i].revents & POLLIN)) {
      fds[i].revents = POLLPRI;
      hasEvent = true;
    }
  }
  if (hasEvent && holdEvent.exchange(false)) {
    std::unique_lock lock(gateMutex);
    eventHeld = true;
    gateCV.notify_all();
    assert(gateCV.wait_for(lock, 1s, [] { return releaseEvent; }));
  }
  return rc;
}

int main() {
  exercise<UdfpsSensor>(true);
  exercise<SingleTapSensor>(false);
  std::cout << "PASS: constructed-before-start, real poll shutdown, full wake pipe, cancelled events and fd cleanup\n";
}
