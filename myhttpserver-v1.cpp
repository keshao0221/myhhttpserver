#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <unordered_map>

#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

namespace {

constexpr int MAX_EVENTS            = 1024;
constexpr int EPOLL_WAIT_MS         = 1;
constexpr int TIMEOUT_CHECK_INTERVAL = 50;
constexpr int MAX_REQUESTS          = 1024;
constexpr int TIMEOUT_SECONDS       = 30;
constexpr int READ_BUF_SIZE         = 4096;
constexpr int DEFAULT_PORT          = 8080;

constexpr const char* HTTP_HEADER_END = "\r\n\r\n";
constexpr size_t     HTTP_HEADER_END_LEN = 4;

constexpr char RESP_KEEPALIVE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 21\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello from my server!";
constexpr size_t RESP_KEEPALIVE_LEN = sizeof(RESP_KEEPALIVE) - 1;

constexpr char RESP_CLOSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 21\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from my server!";
constexpr size_t RESP_CLOSE_LEN = sizeof(RESP_CLOSE) - 1;

}

class HttpServer {
public:
    explicit HttpServer(int port = DEFAULT_PORT)
        : port_(port) {}

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start() {
        if (!init_listen_socket()) return false;
        if (!init_epoll()) {
            close(listen_fd_);
            return false;
        }
        return true;
    }

    void run() {
        struct epoll_event events[MAX_EVENTS];

        while (true) {
            int nfds = epoll_wait(epfd_, events, MAX_EVENTS, EPOLL_WAIT_MS);

            if (++loop_cnt_ >= TIMEOUT_CHECK_INTERVAL) {
                loop_cnt_ = 0;
                check_timeouts();
            }

            if (nfds == -1) {
                std::perror("epoll_wait");
                continue;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    accept_client();
                } else {
                    handle_client(fd, events[i].events);
                }
            }
        }
    }

private:
    struct Connection {
        std::string read_buf;

        struct ResponseRef { const char* data; size_t len; };
        std::vector<ResponseRef> resp_queue;
        size_t resp_idx     = 0;
        size_t resp_off     = 0;
        int request_cnt     = 0;
        time_t last_active  = 0;
        bool keep_alive     = true;
    };

    bool init_listen_socket() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::perror("socket");
            return false;
        }

        if (fcntl(listen_fd_, F_SETFL, O_NONBLOCK) == -1) {
            std::perror("fcntl");
            close(listen_fd_);
            return false;
        }

        int opt = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::perror("setsockopt");
            close(listen_fd_);
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_fd_,
                 reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            std::perror("bind");
            close(listen_fd_);
            return false;
        }

        if (listen(listen_fd_, SOMAXCONN) < 0) {
            std::perror("listen");
            close(listen_fd_);
            return false;
        }

        return true;
    }

    bool init_epoll() {
        epfd_ = epoll_create1(0);
        if (epfd_ == -1) {
            std::perror("epoll_create1");
            return false;
        }

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) == -1) {
            std::perror("epoll_ctl: listen_fd");
            close(epfd_);
            return false;
        }

        return true;
    }

    void accept_client() {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd_,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd == -1) {
            if (errno == EMFILE || errno == ENFILE) {
                pause_accept();
            }
            return;
        }

        if (fcntl(client_fd, F_SETFL, O_NONBLOCK) == -1) {
            std::perror("fcntl client");
            close(client_fd);
            return;
        }

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = client_fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev);

        auto& con = connections_[client_fd];
        con.last_active = time(nullptr);
    }

    void close_connection(int fd) {
        close(fd);
        connections_.erase(fd);
        resume_accept();
    }

    void pause_accept() {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, listen_fd_, nullptr);
        accept_paused_ = true;
    }

    void resume_accept() {
        if (!accept_paused_) return;

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = listen_fd_;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) == 0) {
            accept_paused_ = false;
        }
    }

    void check_timeouts() {
        time_t now = time(nullptr);
        for (auto it = connections_.begin(); it != connections_.end(); ) {
            if (now - it->second.last_active > TIMEOUT_SECONDS) {
                close(it->first);
                it = connections_.erase(it);
                resume_accept();
            } else {
                ++it;
            }
        }
    }

    void handle_client(int client_fd, uint32_t events) {
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) {
            close_connection(client_fd);
            return;
        }

        Connection& con = it->second;

        if (events & (EPOLLERR | EPOLLHUP)) {
            close_connection(client_fd);
            return;
        }

        if (events & EPOLLIN) {
            handle_read(con, client_fd);
        }

        if (events & EPOLLOUT) {
            handle_write(con, client_fd);
        }
    }

    void handle_read(Connection& con, int client_fd) {
        char buf[READ_BUF_SIZE];
        ssize_t n = read(client_fd, buf, sizeof(buf));

        if (n > 0) {
            con.read_buf.append(buf, n);
            con.last_active = time(nullptr);

            bool has_response = false;

            while (true) {
                size_t pos = con.read_buf.find(HTTP_HEADER_END);
                if (pos == std::string::npos) break;

                size_t header_end = pos + HTTP_HEADER_END_LEN;

                bool client_close =
                    con.read_buf.find("Connection: close") < header_end;

                con.request_cnt++;

                bool should_close =
                    client_close || (con.request_cnt >= MAX_REQUESTS);
                con.keep_alive = !should_close;

                con.resp_queue.push_back(
                    should_close
                        ? Connection::ResponseRef{RESP_CLOSE, RESP_CLOSE_LEN}
                        : Connection::ResponseRef{RESP_KEEPALIVE, RESP_KEEPALIVE_LEN});
                has_response = true;

                con.read_buf.erase(0, header_end);
            }

            if (has_response) {
                set_events(client_fd, EPOLLOUT);
            }
        }
        else if (n == 0) {
            close_connection(client_fd);
        }
        else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::perror("read");
                close_connection(client_fd);
            }
        }
    }

    void handle_write(Connection& con, int client_fd) {
        while (con.resp_idx < con.resp_queue.size()) {
            auto& resp = con.resp_queue[con.resp_idx];
            ssize_t n;
            do {
                n = write(client_fd, resp.data + con.resp_off,
                          resp.len - con.resp_off);
            } while (n == -1 && errno == EINTR);

            if (n > 0) {
                con.resp_off += n;
                if (con.resp_off == resp.len) {
                    con.resp_idx++;
                    con.resp_off = 0;
                } else {
                    return;
                }
            } else if (n == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::perror("write");
                    close_connection(client_fd);
                }
                return;
            } else {
                close_connection(client_fd);
                return;
            }
        }

        if (!con.keep_alive) {
            close_connection(client_fd);
        } else {
            con.resp_queue.clear();
            con.resp_idx = 0;
            set_events(client_fd, EPOLLIN);
        }
    }

    void set_events(int fd, uint32_t events) {
        struct epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }

    int listen_fd_ = -1;
    int epfd_      = -1;
    int port_;

    bool accept_paused_ = false;
    int  loop_cnt_      = 0;

    std::unordered_map<int, Connection> connections_;
};

int main() {
    HttpServer server;
    if (!server.start()) return 1;
    server.run();
}
