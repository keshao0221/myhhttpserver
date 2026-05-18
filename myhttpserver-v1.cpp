#include<iostream>
#include<cstring>
#include<unistd.h>
#include<arpa/inet.h>
#include<string>
#include<sstream>
#include<fcntl.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<unordered_map>

const int MAX_REQUESTS=1024;
const int TIMEOUT=30;
const int MAX_EVENTS=1024;

// 全局变量，用于非阻塞恢复监听
int epfd_global = -1;
int listen_fd_global = -1;
bool listen_paused = false;

// 恢复监听辅助函数
void try_resume_listen() {
    if (listen_paused) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_global;
        if (epoll_ctl(epfd_global, EPOLL_CTL_ADD, listen_fd_global, &ev) == 0) {
            listen_paused = false;
        }
    }
}

struct Connection{
  std::string read_buf;
  std::string write_buf;
  size_t write_off=0;
  int request_cnt=0;
  time_t last_active;
  bool keep_alive=1;

  void reset(){
    read_buf.clear();
    write_buf.clear();
    write_off=0;
  }
};

std::unordered_map<int,Connection> connections;

void handle_client(int client_fd,uint32_t events,int epfd){
  auto it=connections.find(client_fd);
  if(it==connections.end()){
    close(client_fd);
    try_resume_listen();
    return;
  }
  Connection& con=it->second;

  // 新增：处理错误挂起事件
  if(events & (EPOLLERR | EPOLLHUP)){
    std::cout<<"fd="<<client_fd<<" 异常断开\n";
    close(client_fd);
    connections.erase(it);
    try_resume_listen();
    return;
  }

  if(events & EPOLLIN){
    char buf[4096];
    ssize_t n=read(client_fd,buf,sizeof(buf)-1);

    if(n>0){
      buf[n]='\0';
      con.read_buf.append(buf,n);

      while(1){
        size_t pos=con.read_buf.find("\r\n\r\n");
        if(pos==std::string::npos)break;

        bool client_close=0;
        size_t search_end=pos+4;
        if(con.read_buf.find("Connection: close")<search_end)client_close=1;

        con.request_cnt++;
        con.last_active=time(nullptr);

        bool if_close=client_close||(con.request_cnt>=MAX_REQUESTS);
        std::string keep_alive_header =if_close?"Connection: close\r\n":
                                                "Connection: keep-alive\r\n";

        std::string body="Hello from my server!";
        std::string response="HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: "+ std::to_string(body.size())+"\r\n"+
                              keep_alive_header+
                              "\r\n"+
                              body;
        
        con.keep_alive=!if_close;

        con.write_buf=response;
        con.write_off=0;

        struct epoll_event ev;
        ev.events=EPOLLOUT;
        ev.data.fd=client_fd;
        epoll_ctl(epfd,EPOLL_CTL_MOD,client_fd,&ev);

        con.read_buf.erase(0,pos+4);
      }
    }else if(n==0){
      std::cout<<"fd="<<client_fd<<"断开连接\n";
      close(client_fd);
      connections.erase(it);
      try_resume_listen();
    }else{
      if(errno!=EAGAIN&&errno != EWOULDBLOCK){
        std::perror("read error\n");
        close(client_fd);
        connections.erase(it);
        try_resume_listen();
      }
    }
  }
  if(events & EPOLLOUT){
    ssize_t n=write(client_fd,
                    con.write_buf.data()+con.write_off,
                    con.write_buf.size()-con.write_off);
    if(n>0){
      con.write_off+=n;
      if(con.write_off==con.write_buf.size()){
        if(!con.keep_alive){
          std::cout<<"响应发送完毕 fd="<<client_fd<<" 关闭连接\n";
          close(client_fd);
          connections.erase(it);
          try_resume_listen();
        }else{
          // 重置写缓冲区
          con.write_buf.clear();
          con.write_off = 0;
          // 检查读缓冲区，避免等待下一次 EPOLLIN
          if (con.read_buf.find("\r\n\r\n") != std::string::npos) {
              handle_client(client_fd, EPOLLIN, epfd);
          } else {
              struct epoll_event ev;
              ev.events = EPOLLIN;
              ev.data.fd = client_fd;
              epoll_ctl(epfd, EPOLL_CTL_MOD, client_fd, &ev);
          }
        }
      }
    }else if(n==-1){
      if(errno!=EAGAIN&&errno!=EWOULDBLOCK){
        std::perror("write error\n");
        close(client_fd);
        connections.erase(it);
        try_resume_listen();
      }
    }
  }
}

int main(){

  std::cout<<"-----服务器开始运行-----\n";
  int listen_fd=socket(AF_INET,SOCK_STREAM,0);
  if(listen_fd<0){
    std::perror("listen error\n");
    return 1;
  }

  if(fcntl(listen_fd,F_SETFL,O_NONBLOCK)==-1)std::perror("fcntl failed\n");

  int opt=1;
  if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0){
    std::perror("setsock failed\n");
    close(listen_fd);
    return 1;
  }

  struct sockaddr_in addr;
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons(8080);
  addr.sin_addr.s_addr=INADDR_ANY;

  if(bind(listen_fd,(struct sockaddr*)&addr,sizeof(addr))<0){
    std::perror("bind failed\n");
    close(listen_fd);
    return 1;
  }

  if(listen(listen_fd,SOMAXCONN)<0){
    std::perror("listen failed\n");
    close(listen_fd);
    return 1;
  }

  int epfd=epoll_create1(0);
  if(epfd==-1){
    std::perror("setepoll failed\n");
    return 1;
  }

  // 赋值全局变量
  epfd_global = epfd;
  listen_fd_global = listen_fd;

  struct epoll_event ev;
  ev.events=EPOLLIN;
  ev.data.fd=listen_fd;
  epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);

  struct epoll_event events[MAX_EVENTS];

  int check_itvl=50;
  int loop_cnt=0;
  while(1){
    int nfds=epoll_wait(epfd,events,MAX_EVENTS,1);//

    if(++loop_cnt>=check_itvl){
      loop_cnt=0;
      time_t now=time(nullptr);

      for(auto it=connections.begin();it!=connections.end();){
        if(now - it->second.last_active>TIMEOUT){
          std::cout<<"连接超时，关闭 fd="<<it->first<<"\n";
          close(it->first);
          it=connections.erase(it);
          try_resume_listen();
        }else{
          it++;
        }
      }
    }
    if(nfds==-1){
      std::perror("epollwait failed\n");
      continue;
    }

    for(int i=0;i<nfds;i++){
      if(events[i].data.fd==listen_fd){
        struct sockaddr_in client_addr;
        socklen_t client_len=sizeof(client_addr);
        int client_fd=accept(listen_fd,(struct sockaddr*)&client_addr,&client_len);
        if(client_fd==-1){
          if(errno==EMFILE||errno==ENFILE){
            epoll_ctl(epfd,EPOLL_CTL_DEL,listen_fd,nullptr);
            listen_paused=true;          // 非阻塞暂停
          }
          continue;
        }

        if(fcntl(client_fd,F_SETFL,O_NONBLOCK)==-1){
          std::perror("setclient failed\n");
          close(client_fd);
          continue;
        }

        struct epoll_event client_ev;
        client_ev.events=EPOLLIN;
        client_ev.data.fd=client_fd;
        epoll_ctl(epfd,EPOLL_CTL_ADD,client_fd,&client_ev);

        connections[client_fd]=Connection();
        connections[client_fd].last_active=time(nullptr);

        std::cout<<"新连接 fd="<<client_fd<<"\n";
      }else{
        int client_fd=events[i].data.fd;
        handle_client(client_fd,events[i].events,epfd);
      }
    }
  }
}
