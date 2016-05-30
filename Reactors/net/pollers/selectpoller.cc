#include "selectpoller.h"
#include <errno.h>
#include <poll.h>
#include<stdio.h>
#include<stdlib.h>
using namespace muduo;

SelectPoller::SelectPoller()
{
    FD_ZERO(&readfds_);
    FD_ZERO(&writefds_);
    max_fd_ = 3;
}
void SelectPoller::FillActiveChannels(fd_set& readset, fd_set& writeset, ChannelList& active_channels)
{
    for(list<int>::iterator begin = fds_.begin(); begin != fds_.end(); begin++){
        int flag = 0;
        Channel* channel = channels_[*begin];
        if(FD_ISSET(*begin, &readset)){
            flag |= kReadEvent;
            channel->set_revents(kReadEvent);
        }
        
        if(FD_ISSET(*begin, &writeset)){
            flag |= kWriteEvent;
            channel->set_revents(kWriteEvent);
        }

        if(flag){
            active_channels.push_back(channel);
        }
    }


}

Timestamp SelectPoller::Poll(int timeout_ms, ChannelList& active_channels)
{
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    //memcpy(&read_set, &readfds_, sizeof(fd_set));
    //memcpy(&write_set, &writefds_, sizeof(fd_set));
    for(list<int>::iterator it = fds_.begin(); it != fds_.end(); it++){
        if(FD_ISSET(*it, &readfds_))
            FD_SET(*it, &read_set);
        if(FD_ISSET(*it, &writefds_))
            FD_SET(*it, &write_set);
    }
    struct timeval timeout;
    timeout.tv_sec = timeout_ms/1000;
    timeout.tv_usec = (timeout_ms%1000)*1000;
    
    int ret;
    ret = select(max_fd_ + 1, &read_set, &write_set, NULL, &timeout);
    Timestamp now(Timestamp::now());
    
    if(ret > 0){
        cout << "some event has happen, ret: " << ret << endl;
        FillActiveChannels(read_set, write_set, active_channels);
    }else if(ret == 0){
        cout << "nothing happended"<< endl;
    }else {
       //cout << "ret : " << ret << endl;
       if(errno != EINTR){
            perror("select error happen!");
    //            cout << "select error happen!, errno: " << errno <<endl;
       }
    }
    return now;
}

bool SelectPoller::UpdateChannel(Channel* channel)
{
    //add one new channel
    if(channel->index() < 0){
        channels_[channel->fd()] = channel;
        //record the fd into readset , writeset
        if(channel->events() & kReadEvent){
            FD_SET(channel->fd(), &readfds_);
        }

        if(channel->events() & kWriteEvent){
            FD_SET(channel->fd(), &writefds_);
        }
        channel->set_index(1);

        if(channel->fd() > max_fd_){
            max_fd_ = channel->fd();
        }
        fds_.push_back(channel->fd());
    }else{
        channels_[channel->fd()] = channel;
        //update readset, writeset  
        if(channel->events() & kReadEvent){
            FD_SET(channel->fd(), &readfds_);
        }else{
            FD_CLR(channel->fd(), &readfds_);
        }

        if(channel->events() & kWriteEvent){
            FD_SET(channel->fd(), &writefds_);
        }else{
            FD_CLR(channel->fd(), &writefds_);
        }

    }

    return true;
}
bool SelectPoller::RemoveChannel(Channel* channel)
{
    channels_.erase(channel->fd());  
    fds_.remove(channel->fd());
    FD_CLR(channel->fd(), &readfds_);
    FD_CLR(channel->fd(), &writefds_);
}
