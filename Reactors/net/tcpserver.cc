#include "tcpserver.h"
#include<stdio.h>
#include<stdlib.h>
#include <arpa/inet.h>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#define BACKLOG 20
TcpServer::TcpServer():actor_(new Actor())
{
}

void TcpServer::SetOptions(Options& option)
{
    options_ = option;
}

int TcpServer::Start()
{
    //start background threads
    for(int i = 0; i < threads_.size(); i++)
        threads_[i].Run();

    //add listen socket into local event loop
    int listenfd;
    struct sockaddr_in server;
    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1){
        perror("create socket failed");
        return -1;
    }

    int opt = SO_REUSEADDR;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
            &opt, sizeof(opt));
    bzero(&server, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(options_.port_);
    server.sin_addr.s_addr = inet_addr(options_.addr_.c_str());

    if(bind(listenfd, (struct sockaddr*)&server, sizeof(server)) == -1){
        perror("binderror");
        return -1;
    }

    if(listen(listenfd, BACKLOG) == -1){
        perror("listenerror");
        return -1;
    }
    listen_fd_ = listenfd;
    accept_channel_.reset(new Channel(actor_.get(), listenfd));   
    
    //set accept channel callback
    accept_channel_->setReadCallback(
            boost::bind(&TcpServer::HandleReadEvent, this, _1));
    //accept_channel_->setCloseCallBack();
    //accept_channel_->setErrorCallBack();
    actor_->UpdateChannel(accept_channel_.get());
    

    state_ = RUNNING;
    //start master evnet loop
    actor_->Loop();
}

void TcpServer::HandleReadEvent(Timestamp ts)
{
   struct sockaddr_in client;
   socklen_t addrlen;

   addrlen = sizeof(client);
   int connfd = accept(listen_fd_, (struct sockaddr*)&client, &addrlen);
   if(connfd >= 0){
        NewConnectionEstablished(connfd, client);
   }else{
       //error process
   }
}

void TcpServer::NewConnectionEstablished(int fd,struct sockaddr_in &peer) 
{
    cout << "conn fd: " << fd << " has been connected" << endl;
    cout << "remote ip: " << inet_ntoa(peer.sin_addr) << " port: " << ntohs(peer.sin_port) << endl; 
}
void TcpServer::Stop()
{
}
