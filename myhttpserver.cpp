#include<iostream>
#include<cstring>
#include<unistd.h>
#include<arpa/inet.h>
#include<string>
#include<sstream>
#include"cppjson.h"

void handle_client(int client_fd){
  //读取请求
  char buf[4096];
  ssize_t n=read(client_fd,buf,sizeof(buf)-1);
  if(n<=0){
    close(client_fd);
    return;
  }

  buf[n]='\0';
  std::string request(buf,n);
  std::cout<<"收到请求："<<request<<"\n";
  //解析路径
  size_t m_end=request.find(' ');
  size_t u_start=m_end+1;
  size_t u_end=request.find(' ',u_start);
  std::string path=request.substr(u_start,u_end-u_start);
  std::cout<<"请求路径："<<path<<"\n";
  //分配路由
  std::string status_line,content_type,body;

  if(path=="/"||path=="/index.html"){
    status_line="HTTP/1.1 200 OK\r\n";
    content_type="Content-Type: text/html;charset=utf-8\r\n";
    body="<html><body><h1>Hello from my first server!</h1></body></html>";
  }else if(path=="/api/hello"){
    status_line="HTTP/1.1 200 OK\r\n";
    content_type="Content-Type: application/json\r\n";
    
    //body="{\"message\":\"Hello JSON from my server!\"}";
    

    //选择手写的json解析库
    JsonValue json_obj;
    json_obj.type=JSON_OBJ;
    json_obj["message"]=JsonValue("Hello form my server!");

    std::ostringstream oss;
    print_value(json_obj,oss);
    body=oss.str();

  }else{
    status_line="HTTP/1.1 404 NOT FOUND\r\n";
    content_type="Content-Type: text/html;charset=utf-8\r\n";
    body="<html><body><h1>404 page not found</h1></body></html>";
  }
  //拼接http响应
  std::string response=status_line+
                        content_type+
                        "Connection: close\r\n"+
                        "Content-Length:"+std::to_string(body.size())+"\r\n\r\n"+
                        body;                  
  //发送响应
  write(client_fd,response.c_str(),response.size());
  //关闭
  shutdown(client_fd,SHUT_WR);
  char dummy[256];
  while(read(client_fd,dummy,sizeof(dummy))>0){};
  close(client_fd);
}
int main(){
  std::cout<<"-----启动服务器-----\n";

  //准备socket
  int listen_fd=socket(AF_INET,SOCK_STREAM,0);
  if(listen_fd<0){
    std::perror("socket error\n");
    return 1;//
  }

  //设置端口复用
  int opt=1;
  if(setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt))<0){
    std::perror("setsockop failed");
    close(listen_fd);
    return 1;
  }

  //绑定地址
  struct sockaddr_in addr;
  std::memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons(8080);
  addr.sin_addr.s_addr=INADDR_ANY;

  if(bind(listen_fd,(struct sockaddr*)&addr,sizeof(addr))<0){
    std::perror("bind error");
    close(listen_fd);
    return 1;
  }

  //开始监听
  if(listen(listen_fd,10)<0){
    std::perror("listen error");
    close(listen_fd);
    return 1;
  }
  std::cout<<"服务器已启动，监听端口为：8080...\n";

  while(1){
    int client_fd=accept(listen_fd,nullptr,nullptr);
    if(client_fd<0){
      std::perror("accept failed");
      continue;
    }

    //读取请求
    handle_client(client_fd);
  }
  close(listen_fd);
}
 