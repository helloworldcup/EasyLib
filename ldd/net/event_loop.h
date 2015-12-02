// event_loop.h (2013-08-08)
// Li Xinjie (xjason.li@gmail.com)

#ifndef LDD_NET_EVENT_LOOP_H_
#define LDD_NET_EVENT_LOOP_H_

#include <event2/event.h>
#include <boost/noncopyable.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <ldd/util/atomic.h>
#include <ldd/util/thread.h>

namespace ldd {
namespace net {

class EventLoop : private boost::noncopyable {
public:
    class Impl;
    typedef boost::function<void()> Functor;
    EventLoop();
    explicit EventLoop(struct ::event_base *base);
    ~EventLoop();

    void Run();
    void Stop();
    void AfterFork();

    void RunInLoop(const Functor& handler);
    void QueueInLoop(const Functor& handler);
    struct event_base *event_base();
private:
    friend class FdEvent;
    friend class SignalEvent;
    friend class TimerEvent;
    friend class Channel;
    friend class Listener;
    friend class Server;
    friend class Client;
    Impl* impl_;
};

class EventLoopThread : private boost::noncopyable {
public:
    typedef boost::function<void()> Functor;
    EventLoopThread();
    ~EventLoopThread();

    bool Start(const Functor& pre = NULL, const Functor& post = NULL);
    void Stop();
    void Join();

    EventLoop* event_loop() const { return event_loop_.get(); }
    const util::Thread* thread() const { return thread_.get(); }
private:
    void Run(const Functor& pre, const Functor& post);
    enum State {
        kStarting,
        kRunning,
        kStopping,
        kJoining,
        kStopped,
    };
    boost::scoped_ptr<EventLoop> event_loop_;
    boost::scoped_ptr<util::Thread> thread_;
    util::Atomic<State> state_;
};


} // namespace net
} // namespace ldd

#endif // LDD_NET_EVENT_LOOP_H_
