// monitor.cc (2013-08-01)
// Li Xinjie (xjason.li@gmail.com)

#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <boost/foreach.hpp>
#include <glog/logging.h>
#include <ldd/util/thread.h>
#include <ldd/util/time.h>
#include "monitor.h"
#include "option.h"
#include "config.h"
#include "child.h"
#include "proctitle.h"
#include "log_flusher.h"

namespace ldd {
namespace skel {

using namespace std;
using namespace boost;

Monitor::Monitor()
    : child_(0),
      new_child_(0)
{
}

Monitor::~Monitor() {
}

bool Monitor::Init(int argc, char** argv) {
    config_.reset(new Config);
    std::string what;
    if (!config_->Init(&what)) {
        cerr << "Config: " << what << endl;
        return false;
    }
    FLAGS_max_log_size = config_->log_size();
    stringstream title;
    title << "lddskel(" << Option::instance().name() << "): monitor process";
    for (int i = 0; i < argc; ++i) {
        title << " " << argv[i];
    }
    if (!ProcTitle::Init(argc, argv)) {
        cerr << "Can't init proc title" << endl;
        return false;
    }
    ProcTitle::Set(title.str());
    return true;
}

void Monitor::Run() {
    mloop_.reset(new net::EventLoop);
    flusher_.reset(new LogFlusher(mloop_.get(), config_->log_flush()));
    cloop_.reset(new net::EventLoop);
    BOOST_FOREACH(int port, config_->ports()) {
        net::Listener* listener = new net::Listener(cloop_.get());
        net::Endpoint ep(config_->addr(), port);
        if (!listener->Open(ep)) {
            delete listener;
            google::FlushLogFiles(0);
            exit(1);
        }
        listeners_.insert(port, listener);
    }
    WatchSignals();
    RunMasters();
    Loop();//event_base_dispatch
    if (config_->type() == Module::kTypeSingleMaster) {
        StopChild();
    } else {
        StopChildren();
    }
}

void Monitor::HandleSignal(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
        LOG(INFO) << "SIGTERM received";
        HandleStop();
        break;
    case SIGCHLD:
        LOG(INFO) << "SIGCHLD received";
        if (config_->type() == Module::kTypeSingleMaster) {
            HandleChild();
        } else {
            HandleChildren();
        }
        break;
    case SIGHUP:
        LOG(INFO) << "SIGHUP recevied";
        HandleReload();
        break;
    }
    sigev_->AsyncWait(
            boost::bind(&Monitor::HandleSignal, this, _1));
}

void Monitor::HandleStop() {
    mloop_->Stop();
}

void Monitor::HandleChild() {
    while (true) {
        int status = 0;
        pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            //PLOG(ERROR) << "waitpid()";
            return;
        } else if (pid == 0) {
            //LOG(ERROR) << "No child was stopped";
            return;
        }

        bool need_kill_grandchildren = false;
        stringstream ss;
        if (pid == new_child_) {
            ss << "new ";
        } else if (pid == child_) {
            if (new_child_ != 0) {
                ss << "old ";
            } else {
                ss << "working ";
            }
        } else if (IsDropped(pid)) {
            ss << "dropped ";
        } else {
            ss << "abandoned ";
        }
        ss << "child on " << pid << " ";
        if (WIFEXITED(status)) {
            ss << "exited with " << WEXITSTATUS(status);
            if (WEXITSTATUS(status) != 0) {
                need_kill_grandchildren = true;
            }
        } else if (WIFSIGNALED(status)) {
            ss << "killed by signal " << WTERMSIG(status);
            if (WTERMSIG(status) != SIGTERM) {
                need_kill_grandchildren = true;
            }
        } else if (WCOREDUMP(status)) {
            ss << "core dumped";
            need_kill_grandchildren = true;
        } else {
            LOG(WARNING) << ss << "just stopped or continued";
            continue;
        }
        
        if (need_kill_grandchildren) {
            kill(-pid, SIGKILL);
        }

        if (pid == new_child_) {
            CHECK(child_);

            new_child_ = 0;
            rconfig_.reset();
            event_.reset();

            LOG(WARNING) << ss.str() << ", keep back old child " << child_;
        } else if (pid == child_) {
            if (new_child_ != 0) {
                child_ = new_child_;
                new_child_ = 0;
                config_ = rconfig_;
                rconfig_.reset();
                event_.reset();

                LOG(WARNING) << ss.str() << ", force use new child "
                    << new_child_;
            } else {
                child_ = 0;
                if (IsDropped(pid)) {
                    rconfig_.reset();
                    LOG(WARNING) << ss.str() << ", reload";
                    RunMaster();
                } else {
                    if (config_->auto_restart()) {
                        LOG(WARNING) << ss.str() << ", reboot";
                        reboot_timer_.reset(new net::TimerEvent(mloop_.get()));
                        reboot_timer_->AsyncWait(
                                boost::bind(&Monitor::RunMaster, this),
                                util::TimeDiff::Seconds(config_->reboot_delay())
                                );
                    } else {
                        LOG(WARNING) << ss.str();
                    }
                }
            }
        } else {
            LOG(WARNING) << ss.str();
        }

        ClearDropped(pid);
    }
}

void Monitor::HandleChildren() {
    while (true) {
        int status = 0;
        pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            //PLOG(ERROR) << "waitpid()";
            return;
        } else if (pid == 0) {
            //LOG(ERROR) << "No child was stopped";
            return;
        }

        bool need_kill_grandchildren = false;

        stringstream ss;
        int port = 0;
        std::map<pid_t, int>::iterator it = children_.find(pid);
        if (it != children_.end()) {
            if (IsDropped(pid,&port)) {
                ss << "old ";
            } else {
                ss << "working ";
                port = it->second;
            }
        } else if (IsDropped(pid, &port)) {
            ss << "dropped ";
        } else {
            ss << "abandoned ";
        }
        ss << "child ["
            << (port ?
                boost::lexical_cast<std::string>(port) : "?")
            << "] on " << pid << " ";
        if (WIFEXITED(status)) {
            ss << "exited with " << WEXITSTATUS(status);
            if (WEXITSTATUS(status) != 0) {
                need_kill_grandchildren = true;
            }
        } else if (WIFSIGNALED(status)) {
            ss << "killed by signal " << WTERMSIG(status);
            if (WTERMSIG(status) != SIGTERM) {
                need_kill_grandchildren = true;
            }
        } else if (WCOREDUMP(status)) {
            ss << "core dumped";
            need_kill_grandchildren = true;
        } else {
            LOG(WARNING) << ss << "just stopped or continued";
            continue;
        }

        if (need_kill_grandchildren) {
            kill(-pid, SIGKILL);
        }

        if (it != children_.end()) {
            children_.erase(it);
            if (IsDropped(pid)) {
                CHECK(port);
                LOG(WARNING) << ss.str() << ", reload";
                RunMaster(port);
                bool success = true;
                typedef std::map<pid_t, int>::value_type PidAndPort;
                BOOST_FOREACH(PidAndPort& x, children_) {
                    if (IsDropped(x.first)) {
                        success = false;
                    }
                }
                if (success) {
                    rconfig_.reset();
                    LOG(WARNING) << "all children reload success";
                }
            } else {
                if (config_->auto_restart()) {
                    LOG(WARNING) << ss.str() << ", reboot";
                    std::auto_ptr<net::TimerEvent> timer(
                            new net::TimerEvent(mloop_.get()));
                    timer->AsyncWait(
                            boost::bind(&Monitor::RunMaster, this, port),
                            util::TimeDiff::Seconds(config_->reboot_delay()));

                    reboot_timers_.insert(port, timer);
                } else {
                    LOG(WARNING) << ss.str();
                }
            }
        } else {
            LOG(WARNING) << ss.str();
        }
        ClearDropped(pid);
    }
}

void Monitor::HandleReload() {
    if (rconfig_) {
        LOG(INFO) << "Previous reload is in progress, ignore this reload";
        return;
    }
    boost::shared_ptr<Config> cfg(new Config);
    std::string what;
    if (!cfg->Init(&what)) {
        LOG(ERROR) << "Reload failed: " << what;
        return;
    }
    if (cfg->type() != config_->type()) {
        LOG(ERROR) << "Can't reload: changing module type is not supported";
        return;
    }
    if (cfg->addr() != config_->addr()) {
        LOG(ERROR) << "Can't reload: changing addr is not supported";
        return;
    }
    if (cfg->ports() != config_->ports()) {
        LOG(ERROR) << "Can't reload: changing port is not supported";
        return;
    }
    rconfig_ = cfg;

    LOG(INFO) << "Reloading...";
    DoReload();
    //timers_.clear();
}

void Monitor::DoReload() {
    CHECK(rconfig_);
    config_ = rconfig_;

    flusher_->set_seconds(config_->log_flush());
    FLAGS_minloglevel = config_->log_level();
    FLAGS_max_log_size = config_->log_size();

    if (rconfig_->type() == Module::kTypeSingleMaster) {
        ReloadSingleChild();
    } else {
        ReloadMultiChild();
    }
}

void Monitor::ReloadSingleChild() {
    CHECK(rconfig_);
    if (child_ == 0) {
        reboot_timer_.reset();
        rconfig_.reset();
        RunMaster();
        return;
    }
    CHECK(!reboot_timer_);
    switch (rconfig_->reload_policy()) {
    case Reload::kPolicySimple:
        DropChild(child_);
        child_ = 0;
        rconfig_.reset();
        RunMaster();
        break;
    case Reload::kPolicySafe:
        RunMaster();
        break;
    case Reload::kPolicySole:
        DropChild(child_);
        break;
    default:
        LOG(FATAL) << "INVALID reload policy = " << rconfig_->reload_policy();
    }
}

void Monitor::ReloadMultiChild() {
    CHECK(rconfig_);
    if (children_.empty()) {
        reboot_timers_.clear();
        rconfig_.reset();
        RunMasters();
        return;
    }
    switch (rconfig_->reload_policy()) {
    case Reload::kPolicySafe:
        // not supported, use simply policy
    case Reload::kPolicySimple:
        typedef std::map<pid_t, int>::value_type PidAndPort;
        BOOST_FOREACH(PidAndPort& x, children_) {
            DropChild(x.first, x.second);
        }
        children_.clear();
        reboot_timers_.clear();
        rconfig_.reset();
        RunMasters();
        break;
    case Reload::kPolicySole:
        typedef std::map<pid_t, int>::value_type PidAndPort;
        if (children_.empty()) {
            rconfig_.reset();
            LOG(WARNING) << "No live children, sole reload success";
        } else {
            std::set<int> ports = rconfig_->ports();
            BOOST_FOREACH(PidAndPort& x, children_) {
                DropChild(x.first, x.second);
                ports.erase(x.second);
            }

            BOOST_FOREACH(int port, ports) {
                RunMaster(port);
            }
        }
        break;
    default:
        LOG(FATAL) << "INVALID reload policy = " << rconfig_->reload_policy();
    }
}

void Monitor::WatchSignals() {
    sigev_.reset(new net::SignalEvent(mloop_.get()));
    sigev_->Add(SIGINT);
    sigev_->Add(SIGTERM);
    sigev_->Add(SIGQUIT);
    sigev_->Add(SIGHUP);
    sigev_->Add(SIGCHLD);
    sigev_->AsyncWait(
            boost::bind(&Monitor::HandleSignal, this, _1));
}

void Monitor::RunMasters() {
    if (config_->type() == Module::kTypeSingleMaster) {
        LOG(INFO) << "Running process...";
        RunMaster();
    } else {
        BOOST_FOREACH(int port, config_->ports()) {
            LOG(INFO) << "Running process for " << port << "...";
            RunMaster(port);
        }
    }
}

void Monitor::RunMaster() {
    CHECK_EQ(new_child_, 0);
    reboot_timer_.reset();

    google::FlushLogFiles(0);
    int fork_pipe[2];
    int init_pipe[2];
    if (pipe(fork_pipe)) {
        PLOG(ERROR) << "create fork_pipe";
        return;
    }
    if (pipe(init_pipe)) {
        PLOG(ERROR) << "create init_pipe";
        close(fork_pipe[0]);
        close(fork_pipe[1]);
        return;
    }
    char buf = 1;
    pid_t pid = fork();
    if (pid < 0) {
        PLOG(ERROR) << "fork child failed";
        return;
    } else if (pid > 0) {
        // in monitor
        close(fork_pipe[0]);
        close(init_pipe[1]);

        // notify child for fork status
        write(fork_pipe[1], &buf, 1);
        close(fork_pipe[1]);
        LOG(INFO) << "forked child on " << pid;

        if(!child_) {
            child_ = pid;
        } else {
            new_child_ = pid;
            int init_timeout = rconfig_->init_timeout();
            event_ .reset(new ldd::net::FdEvent(mloop_.get()));
            event_->AsyncWait(init_pipe[0], net::FdEvent::kReadable,
                    boost::bind(&Monitor::OnSingleMasterInit, this, _1,
                        init_pipe[0]),
                    util::TimeDiff::Seconds(init_timeout));
        }
        return;
    }
    // in child
    close(fork_pipe[1]);
    close(init_pipe[0]);

    // wait fork notify from monitor
    if (read(fork_pipe[0], &buf, 1) != 1) {
        PLOG(ERROR) << "read fork_pipe from child";
        exit(0);
    }
    close(fork_pipe[0]);

    mloop_->AfterFork();
    cloop_->AfterFork();

    sigev_.reset();
    mloop_.reset();
    google::ShutdownGoogleLogging();
    FLAGS_minloglevel = config_->log_level();
    FLAGS_max_log_size = config_->log_size();

    google::InitGoogleLogging("master");
    ChangeUser();
    setpgid(0, 0);
    LOG(INFO) << "master process started";

    Child* child = new SingleMasterChild(config_.get(), cloop_.get(),
            listeners_);
    if (child->Init()) {
        write(init_pipe[1], &buf, 1);
        close(init_pipe[1]);
        child->Run();
    } else {
        buf = 0;
        write(init_pipe[1], &buf, 1);
        close(init_pipe[1]);
    }
    delete child;
    //delete this;
    google::FlushLogFiles(0);
    exit(0);
}

void Monitor::RunMaster(int port) {
    reboot_timers_.erase(port);

    google::FlushLogFiles(0);
    int fd[2];
    pipe(fd);
    char buf = 1;
    pid_t pid = fork();
    if (pid < 0) {
        LOG(ERROR) << "fork child [" << port << "] failed: " << strerror(errno);
        return;
    } else if (pid > 0) {
        // in monitor
        children_[pid] = port;
        close(fd[0]);
        write(fd[1], &buf, 1);
        close(fd[1]);
        LOG(INFO) << "forked child [" << port << "] on " << pid;
        return;
    }
    // in child
    close(fd[1]);
    read (fd[0], &buf, 1);
    close(fd[0]);
    mloop_->AfterFork();
    cloop_->AfterFork();

    sigev_.reset();
    mloop_.reset();
    google::ShutdownGoogleLogging();
    FLAGS_minloglevel = config_->log_level();
    FLAGS_max_log_size = config_->log_size();

    string role;
    if (config_->type() == Module::kTypeWorker) {
        role = "master";
    } else {
        role = "child";
    }
    string logname = role + "-" + lexical_cast<string>(port);
    google::InitGoogleLogging(logname.c_str());
    ChangeUser();
    setpgid(0,0);
    LOG(INFO) << role << " [" << port << "] started";

    ListenerMap::iterator it = listeners_.find(port);
    CHECK(it != listeners_.end());
    net::Listener* listener = listeners_.release(it).release();
    listeners_.clear();
    listeners_.insert(port, listener);

    Child* child = NULL;
    switch (config_->type()) {
    case Module::kTypeSimple:
        child = new SimpleChild(config_.get(), port, listener);
        break;
    case Module::kTypeWorker:
        child = new WorkerChild(config_.get(), port, listener);
        break;
    default:
        LOG(FATAL) << "Can't go here";
    }
    if (child->Init()) {
        child->Run();
    }
    delete child;
    //delete this;
    google::FlushLogFiles(0);
    exit(0);
}

void Monitor::OnSingleMasterInit(int ev, int fd) {
    if (ev & net::FdEvent::kReadable) {
        char buf = 0;
        int n = read(fd, &buf, 1);
        if (n == 1 && buf == 1) {
            LOG(INFO) << "new child on " << new_child_ << " initialize success";
            // new child init ok
            DropChild(child_);
            child_ = new_child_;
            new_child_ = 0;
            rconfig_.reset();
        } else {
            LOG(INFO) << "new child on " << new_child_ << " initialize failed";
        }
    } else {
        LOG(INFO) << "new child on " << new_child_ << " initialize timeout";
        DropChild(new_child_);
        new_child_ = 0;
        rconfig_.reset();
    }
    event_.reset();
}

void Monitor::Loop() {
    mloop_->Run();
    LOG(INFO) << "monitor exiting...";
}

void Monitor::StopChild() {
    LOG(INFO) << "stopping child...";
    kill(child_, SIGTERM);
    int64_t start = util::Time::CurrentMilliseconds();
    while (child_ != 0) {
        int64_t now = util::Time::CurrentMilliseconds();
        if ((now - start) > config_->stop_timeout() * 1000) {
            break;
        }
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            PLOG(ERROR) << "waitpid()";
            break;
        } else if (pid == 0) {
            util::Thread::SleepForMs(100);
            continue;
        }
        if (pid != child_) {
            continue;
        }
        //CHECK_EQ(pid, child_);
        if (WIFEXITED(status) || WIFSIGNALED(status) || WCOREDUMP(status)) {
            LOG(INFO) << "child on " << pid << " finished";
            child_ = 0;
        }
    }
    if (child_ != 0) {
        kill(-child_, SIGKILL);
        LOG(ERROR) << "child on " << child_ << " refuse to die, killed";
    }
}

void Monitor::StopChildren() {
    LOG(INFO) << "stopping children...";
    typedef std::map<pid_t, int>::value_type PidAndPort;
    BOOST_FOREACH(PidAndPort& x, children_) {
        kill(x.first, SIGTERM);
    }
    int64_t start = util::Time::CurrentMilliseconds();
    while (!children_.empty()) {
        int64_t now = util::Time::CurrentMilliseconds();
        if ((now - start) > config_->stop_timeout() * 1000) {
            break;
        }
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0) {
            PLOG(ERROR) << "waitpid()";
            break;
        } else if (pid == 0) {
            util::Thread::SleepForMs(100);
            continue;
        }
        std::map<pid_t, int>::iterator it = children_.find(pid);
        if (it == children_.end()) {
            continue;
        }
        //CHECK(it != children_.end());
        int port = it->second;
        if (WIFEXITED(status) || WIFSIGNALED(status) || WCOREDUMP(status)) {
            LOG(INFO) << "child [" << port << "] on " << pid << " finished";
            children_.erase(pid);
            continue;
        }
    }
    BOOST_FOREACH(PidAndPort& x, children_) {
        int port = x.second;
        pid_t pid = x.first;
        kill(-x.first, SIGKILL);
        LOG(ERROR) << "child [" << port << "] on " << pid
            << " refuse to die, killed";
    }
}

void Monitor::ChangeUser() {
    std::string user = config_->user();
    if (user.empty()) {
        return;
    }
    errno = 0;
    struct passwd* pw = getpwnam(user.c_str());
    if (!pw) {
        if (errno == 0) {
            LOG(ERROR) << user << " can not be found in /etc/passwd";
        } else {
            LOG(ERROR) << "getpwnam: " << strerror(errno);
        }
    }
    if (setgid(pw->pw_gid) == -1) {
        PLOG(ERROR) << "setgid";
    }
    if (initgroups(pw->pw_name, pw->pw_gid) == -1) {
        PLOG(ERROR) << "initgroups";
    }
    if (setuid(pw->pw_uid) == -1) {
        PLOG(ERROR) << "setuid";
    }
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {
        PLOG(ERROR) << "prctl";
    }
}

void Monitor::DropChild(pid_t pid, int port) {
    if (port) {
        LOG(INFO) << "Dropping child [" << port << "] on " << pid;
    } else {
        LOG(INFO) << "Dropping child " << pid;
    }
    if (kill(pid, SIGTERM) && errno == ESRCH) {
        return;
    }
    net::TimerEvent* timer = new net::TimerEvent(mloop_.get());
    dropped_children_.insert(pid, timer);
    if (port) {
        dropped_ports_.insert(std::make_pair(pid, port));
    }
    timer->AsyncWait(boost::bind(&Monitor::OnDropChildTimeout, this, pid),
            util::TimeDiff::Seconds(config_->stop_timeout()));
}

void Monitor::OnDropChildTimeout(pid_t pid) {
    LOG(WARNING) << "drop child " << pid << " timed out, sending SIGKILL";
    kill(-pid, SIGKILL);
}

bool Monitor::IsDropped(pid_t pid, int* port) {
    boost::ptr_map<pid_t, net::TimerEvent>::iterator it = 
        dropped_children_.find(pid);
    if (it != dropped_children_.end()) {
        if (port) {
            if (dropped_ports_.find(pid) != dropped_ports_.end()) {
                *port = dropped_ports_[pid];
            } else {
                *port = 0;
            }
        }
        return true;
    }
    return false;
}

void Monitor::ClearDropped(pid_t pid) {
    dropped_children_.erase(pid);
    dropped_ports_.erase(pid);
}


} // namespace skel
} // namespace ldd
