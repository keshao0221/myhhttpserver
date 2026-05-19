/******************************************************************************
 * myhttpserver.cpp — 基于 epoll 的非阻塞 HTTP 服务器
 *
 * 【改动总览】
 *  原代码：所有变量/函数散落在全局作用域，没有任何封装
 *  改动后：封装为 HttpServer 类，成员变量私有化，公开 start()/run() 接口
 *
 *  为什么封装成类：
 *  - 全局变量在大型项目中极易冲突（其他 .cpp 文件可能定义了同名变量），
 *    类成员被限制在类作用域内，不会污染全局命名空间
 *  - 资源管理更清晰：listen_fd、epfd、connections_ 的生命周期都与
 *    HttpServer 对象绑定，对象销毁时可以统一清理
 *  - 测试友好：未来可以继承或 mock 这个类做单元测试
 ******************************************************************************/

// ============================================================================
//                                  头文件
// ============================================================================
// 【改动】原代码 #include<iostream> 和 #include<cstring> 等缺少空格
//         ——预处理指令 #include 后加空格是 C++ 通用风格，纯格式修正

// C++ 标准库
#include <iostream>          // std::cout
#include <string>            // std::string, std::to_string
// 【优化】原代码: #include <sstream>  —— 删除原因：整个文件中没有任何 ostringstream/
//         stringstream 的使用，纯属无效包含。每个 #include 都会增加编译时间，
//         尤其在大型项目中累积效应明显。注释保留而非直接删除，方便将来需要时取消注释。
// #include <sstream>
// 【优化】原代码: #include <cstring>   —— 删除原因：文件中没有任何 C 字符串函数调用
//         (strlen/strcmp/strcpy 等)，std::string 的方法已覆盖所有字符串操作。
// #include <cstring>
#include <cstdio>            // ★改动：原代码没加这个头文件！原代码用了 std::perror，
                             //   perror 的 C++ 声明在 <cstdio> 中。没加也能编译通过
                             //   是因为 <iostream> 内部间接包含了它，但这是"巧合依赖"——
                             //   不同编译器/版本可能不包含，规范做法是显式 include。
#include <ctime>             // ★改动：原代码没有加 <ctime>！原代码用了 time() 和
                             //   time_t 类型。同样是"巧合依赖"——某些编译器会自动
                             //   包含，但 C++ 标准不保证，必须显式添加。
#include <unordered_map>     // std::unordered_map —— 原代码已有，保持不变
#include <vector>             // std::vector —— 响应队列

// POSIX / Linux 系统头文件 —— 原代码已有，分组排列使其更清晰
#include <unistd.h>          // close(), read(), write()
#include <fcntl.h>           // fcntl() —— 设置文件描述符为非阻塞
#include <arpa/inet.h>       // htons(), sockaddr_in
#include <sys/socket.h>      // socket(), bind(), listen(), accept(), setsockopt()
#include <sys/epoll.h>       // epoll_create1(), epoll_ctl(), epoll_wait()

// ============================================================================
//                          编译期常量 (匿名命名空间)
// ============================================================================
/*
 * 【改动】原代码用的是 #define 宏定义：
 *          const int MAX_REQUESTS=1024;
 *       问题：
 *         1. const int 是运行时变量（虽然编译器常会优化），语义上不是"编译期确定"
 *         2. 定义在全局作用域，其他文件理论上可以 extern 访问
 *       改为：
 *          constexpr int MAX_REQUESTS = 1024;  放在 namespace { } 内
 *
 * 【新关键字】constexpr —— 编译期常量
 *       const 表示"不可修改"，但不保证值在编译期已知。
 *       constexpr 强制编译器在编译期求值——如果值不能在编译期确定，直接报错。
 *       在这个场景下，const 和 constexpr 行为相同，但 constexpr 更准确地表达意图：
 *       "这是一个编译期就能确定的常量，可以用于数组大小等需要编译期常量的场景。"
 *
 * 【新关键字】namespace { } —— 匿名命名空间
 *       作用等同于 C 语言的 static 全局变量：让定义只在本 .cpp 文件内可见。
 *       比 static 更好的是：它也适用于类、结构体、typedef，而不止是变量和函数。
 *       C++ 项目中优先用匿名 namespace 而非 static 来做文件作用域隔离。
 */

namespace {

// --- epoll / 连接管理 ---

constexpr int MAX_EVENTS            = 1024;   // epoll_wait 单次最多返回事件数
constexpr int EPOLL_WAIT_MS         = 1;      // epoll_wait 超时(ms)，原代码硬编码 1
constexpr int TIMEOUT_CHECK_INTERVAL = 50;     // 原代码 check_itvl=50，改名前加注释
constexpr int MAX_REQUESTS          = 1024;   // 单连接最多请求数，原代码同名常量
constexpr int TIMEOUT_SECONDS       = 30;     // 原代码 TIMEOUT=30，改名为更清晰
constexpr int READ_BUF_SIZE         = 4096;   // ★改动：原代码硬编码 4096，
                                               //   提取为常量便于调整
constexpr int DEFAULT_PORT          = 8080;   // ★改动：原代码硬编码端口号，
                                               //   提取后可通过构造函数参数修改

// 【优化】原代码 "\r\n\r\n" 字面量在 handle_read、handle_write 中共出现 3 次。
// 重复字符串字面量有两个问题：
//   1. 编译器可能合并也可能不合并（取决于优化级别和编译器实现），不合并则浪费只读段空间
//   2. 需要修改时（如改为 \n\n），容易漏改其中一处
// 提取为 constexpr 常量后：一处定义，多处引用；语义明确（名字就是注释）。
constexpr const char* HTTP_HEADER_END = "\r\n\r\n";
constexpr size_t     HTTP_HEADER_END_LEN = 4;  // strlen("\r\n\r\n")

// 预计算完整 HTTP 响应：改为 constexpr char[] 数组，
// sizeof() 可在编译期求出长度，避免运行时 strlen。
// 响应队列只存地址 (data + len)，零拷贝。
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

} // namespace

// ============================================================================
//                      HttpServer —— 非阻塞 HTTP 服务器
// ============================================================================
/*
 * 【改动】原代码没有类，所有逻辑都在 main() 和全局函数中。
 *       改为 class HttpServer 后：
 *       - 所有相关函数和变量组织在一起
 *       - 外部只能通过 start() / run() 使用，内部实现细节被 private 隐藏
 *       - 将来可以在一个进程里创建多个 HttpServer 实例（监听不同端口）
 */

class HttpServer {
public:
    // =========================================================================
    //                         构造 / 拷贝控制
    // =========================================================================

    /*
     * 【新关键字】explicit —— 禁止隐式类型转换
     *      构造函数前面加 explicit，防止编译器做自动类型转换。
     *      例子：
     *        void foo(HttpServer s) { ... }
     *        foo(8080);   // 如果没有 explicit，编译器会悄悄把 8080 转成 HttpServer(8080)
     *                     // 加了 explicit 后，这一行直接编译报错，避免意外
     *      实践原则：单参数构造函数几乎都应该加 explicit。
     */
    explicit HttpServer(int port = DEFAULT_PORT)
        : port_(port) {}
    /*
     * 构造函数的 : port_(port) 叫"初始化列表" —— 原代码没用过。
     * 在构造函数体执行之前初始化成员变量，效率更高（直接构造，而非先默认构造再赋值）。
     * 成员变量命名末尾加 _ 下划线是一种常见约定，方便区分成员变量和局部变量。
     */

    /*
     * 【新关键字】= delete —— 显式禁止函数
     *      原代码没有这个需求，因为当时全是全局变量，没有"拷贝整个服务器"的场景。
     *      类封装后，HttpServer 持有文件描述符（epfd_, listen_fd_），
     *      如果允许拷贝，两个 HttpServer 对象会持有同一份 fd，析构时会 close 两次
     *      （double-close），第二次 close 行为未定义（可能关掉其他线程正在用的 fd）。
     *      = delete 告诉编译器：不允许拷贝，有人尝试拷贝就直接报错。
     */
    HttpServer(const HttpServer&) = delete;             // 禁止拷贝构造
    HttpServer& operator=(const HttpServer&) = delete;  // 禁止拷贝赋值

    // =========================================================================
    //                         公开接口
    // =========================================================================

    // 初始化 socket + epoll，成功返回 true。
    // ★改动：原代码初始化全部写在 main() 里，80 行平铺直叙；
    //        现在拆成 start() → init_listen_socket() → init_epoll()，
    //        每层只做一件事，出错立即返回，像"漏斗"一样逐层筛除错误。
    bool start() {
        if (!init_listen_socket()) return false;
        if (!init_epoll()) {
            close(listen_fd_);     // socket 已创建但 epoll 失败，手动关闭
            return false;
        }

        /*
         * 【优化】删除了 listen_fd_global_ / epfd_global_ 成员变量。
         *   原代码（全局变量时代）：
         *     int epfd_global = -1;
         *     int listen_fd_global = -1;
         *   封装为类后保留了这两个成员作为"兼容占位"，在 start() 中赋值但从未被读取。
         *   删除原因：
         *     1. 赋值但从不读取 —— 理想编译器会优化掉，但仍产生维护成本
         *     2. 如果未来外部需要访问，应该通过公开的 getter 方法提供
         *        (如 int listen_fd() const { return listen_fd_; })
         *       而非暴露裸成员变量
         */

        std::cout << "----- 服务器开始运行 (port " << port_ << ") -----\n";
        return true;
    }

    // 进入事件循环，永不返回。
    // ★改动：原代码所有逻辑都在 main() 的一个 while(1) 里，超过 100 行；
    //        现在拆成 run() → accept_client() / handle_client() →
    //        handle_read() / handle_write()，事件循环本身只有 ~30 行。
    void run() {
        struct epoll_event events[MAX_EVENTS];  // 栈分配，约 12KB

        while (true) {
            int nfds = epoll_wait(epfd_, events, MAX_EVENTS, EPOLL_WAIT_MS);

            // 周期性超时扫描：每 TIMEOUT_CHECK_INTERVAL 次循环扫一次
            // 为什么放 epoll_wait 之后？如果放在之前，高负载下 epoll_wait
            // 几乎不阻塞，计数器可能很久才涨一次；放之后保证每次都有机会。
            if (++loop_cnt_ >= TIMEOUT_CHECK_INTERVAL) {
                loop_cnt_ = 0;
                check_timeouts();
            }

            if (nfds == -1) {
                std::perror("epoll_wait");
                continue;           // 非致命错误，继续
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
    // =========================================================================
    //                    Connection —— 每个 TCP 连接的状态
    // =========================================================================
    /*
     * ★改动：原代码 Connection 是一个全局 struct，定义在文件顶部。
     *       现在放到 HttpServer 的 private 区域。
     *       好处：
     *       - 只有 HttpServer 内部需要知道 Connection 结构
     *       - 避免命名冲突（其他文件可以有自己的 Connection 定义）
     *
     * ★改动：原代码成员变量未赋默认值 (int request_cnt; 无初始化)。
     *       改为类内初始化 (int request_cnt = 0;)。
     *       好处：不会出现"忘记赋值就读取"的未定义行为。
     *       原代码依赖 connections_[client_fd] = Connection() 这种写法
     *       来触发零初始化，但正确的类内初值更安全——即使忘记显式初始化，
     *       默认值也是确定的。
     */
    struct Connection {
        std::string read_buf;

        // 响应队列：只存预编译响应的地址 (data, len)，不拷贝内容
        struct ResponseRef { const char* data; size_t len; };
        std::vector<ResponseRef> resp_queue;
        size_t resp_idx     = 0;       // 当前正在发送的响应在队列中的下标
        size_t resp_off     = 0;       // 当前响应已发送字节偏移
        int request_cnt     = 0;       // Keep-Alive 内已处理请求数
        time_t last_active  = 0;       // 最后活跃时间戳
        bool keep_alive     = true;    // 是否保持连接
    };

    // =========================================================================
    //                          初始化
    // =========================================================================
    /*
     * ★改动：原代码所有初始化全在 main() 开头平铺，约 40 行无分段。
     *       现在 socket() / fcntl() / setsockopt() / bind() / listen()
     *       每一步都有独立的错误处理和资源回收，逻辑更清晰。
     *
     * ★改动：错误处理从"打印 + return 1"改为"打印 + return false"，
     *       交给 start() 统一决定是否退出。这样初始化逻辑和退出策略解耦。
     */

    bool init_listen_socket() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            std::perror("socket");
            return false;
        }

        // 设置非阻塞 —— 原代码一致，保留
        if (fcntl(listen_fd_, F_SETFL, O_NONBLOCK) == -1) {
            std::perror("fcntl");
            close(listen_fd_);
            return false;
        }

        // SO_REUSEADDR —— 原代码一致，保留
        int opt = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::perror("setsockopt");
            close(listen_fd_);
            return false;
        }

        /*
         * ★改动：原代码用 memset 清零：
         *          struct sockaddr_in addr;
         *          memset(&addr,0,sizeof(addr));
         *       改为 C++11 的 {} 聚合初始化：
         *          struct sockaddr_in addr{};
         *
         *       为什么：
         *       - {} 是类型安全的——如果结构体有 std::string 成员，memset 会
         *         破坏其内部状态，而 {} 不会。
         *       - {} 永远不会"忘记"——写 memset 需要手动指定 sizeof，
         *         写错就会越界或清不干净，而 {} 总是零初始化整个结构。
         *       - {} 更简洁，一眼就知道是"置零"。
         */
        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port_);       // ★改动：原代码硬编码 htons(8080)
        addr.sin_addr.s_addr = INADDR_ANY;

        /*
         * ★改动：原代码 C 风格强制转换 (struct sockaddr*)&addr
         *       改为 reinterpret_cast<struct sockaddr*>(&addr)
         *
         * 【新关键字】reinterpret_cast —— 重新解释内存类型
         *      这是 C++ 的四种强制转换之一，专门用于"把这段内存当成另一种类型来解释"。
         *      对比 C 风格强转 (struct sockaddr*)，reinterpret_cast：
         *      - 名字很长很丑，这是刻意设计的——提醒读者"这里在做危险的事"
         *      - 更容易 grep 搜索，而 (Type) 这类括号强转极难搜索
         *      - 不会意外触发 const 移除或数值转换（后者由 static_cast 负责）
         *
         *      C++ 四种转换速记：
         *        static_cast       —— 相关类型间的合理转换 (int→double, 父类→子类)
         *        reinterpret_cast  —— 不相关类型间的底层重解释 (整数→指针, 结构体→字节)
         *        const_cast        —— 移除 const 修饰 (尽量别用)
         *        dynamic_cast      —— 运行时多态类型检查 (有虚函数的继承链)
         */
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

    // =========================================================================
    //                       连接生命周期管理
    // =========================================================================
    /*
     * ★改动：原代码中 accept + 设置非阻塞 + 注册 epoll + 创建 Connection 记录
     *        全部内联在 main() 事件循环的 if(events[i].data.fd==listen_fd) 分支里，
     *        约 20 行嵌套在一个 if 内部，阅读困难。
     *       现在全部提取到 accept_client() 方法中，职责单一。
     */

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

        /*
         * 【优化】原代码:
         *     connections_[client_fd] = Connection{};
         *     connections_[client_fd].last_active = time(nullptr);
         *   问题：operator[] 在键不存在时已经默认构造了一个 Connection 对象，
         *   再赋值 Connection{} = 临时对象默认构造 → 移动赋值 → 销毁临时，
         *   等于两次构造+一次析构，纯浪费。
         *   优化后：利用 operator[] 原地默认构造的能力，直接拿到引用改字段，
         *   省略临时对象的创建和销毁。
         */
        auto& con = connections_[client_fd];
        con.last_active = time(nullptr);

        std::cout << "新连接 fd=" << client_fd << '\n';
    }

    /*
     * ★改动：原代码关闭连接的逻辑散落在 4 个不同位置，每次都要写三行：
     *          close(client_fd);
     *          connections.erase(it);
     *          try_resume_listen();
     *       任何一处忘记调用 try_resume_listen() 就会导致 accept 永久卡死。
     *       现在统一到 close_connection() 方法，一步到位，不会遗漏。
     */
    void close_connection(int fd) {
        close(fd);
        connections_.erase(fd);
        resume_accept();
    }

    /*
     * ★改动：原代码是全局函数 try_resume_listen() 和全局变量 listen_paused，
     *       分散在文件顶部。现在封装为 pair: pause_accept() + resume_accept()，
     *       状态由 accept_paused_ 成员管理。
     */
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

    /*
     * ★改动：原代码超时扫描内联在事件循环中，用 if(++loop_cnt>=check_itvl) 包裹，
     *       与事件分发逻辑混在一起。现在提取为独立方法，事件循环只需要一行调用。
     *
     * 【已有关键字但你可能不熟悉】auto —— 自动类型推导
     *      原代码中你用过 auto：没有，你写的是完整的 std::unordered_map<int,Connection>::iterator
     *      改后：
     *        for (auto it = connections_.begin(); ...)
     *      auto 让编译器自动推导 it 的类型为 std::unordered_map<int, Connection>::iterator。
     *      优势：类型名越长，auto 的优势越明显。这行如果不用 auto 会非常长。
     *      注意：auto 不是动态类型！类型在编译期就确定了，和手写完全一样，零运行时开销。
     */
    void check_timeouts() {
        time_t now = time(nullptr);
        for (auto it = connections_.begin(); it != connections_.end(); ) {
            if (now - it->second.last_active > TIMEOUT_SECONDS) {
                std::cout << "连接超时，关闭 fd=" << it->first << '\n';
                close(it->first);
                it = connections_.erase(it);    // erase 返回下一个迭代器，无需 ++
                resume_accept();
            } else {
                ++it;
            }
        }
    }

    // =========================================================================
    //                        HTTP 请求处理核心
    // =========================================================================
    /*
     * ★改动：原代码 handle_client() 和 main() 中事件分发逻辑部分重叠。
     *       现在 handle_client() 纯粹做事件分发：查表 → 判断 ERR/HUP → 分发读写。
     *       真正的读写逻辑在 handle_read() / handle_write() 中。
     *
     * ★改动：原代码 handle_client() 里直接写 close(client_fd) + connections.erase(it)
     *       + try_resume_listen()。现在统一调用 close_connection()。
     */

    void handle_client(int client_fd, uint32_t events) {
        auto it = connections_.find(client_fd);
        /*
         * 【优化】原代码找不到 fd 时手动写了 close + resume_accept：
         *     close(client_fd);
         *     resume_accept();
         *   改为统一使用 close_connection()。
         *   原因：
         *     1. close_connection() 内部也调用了 connections_.erase(fd)，
         *        此处 fd 本就不在 map 中，erase 是无操作，不影响正确性
         *     2. 一致性：整个项目有且仅有一个"关闭连接"的入口，未来如果需要在
         *        关闭时增加日志、统计、限流恢复等逻辑，只改一处即可
         */
        if (it == connections_.end()) {
            close_connection(client_fd);
            return;
        }

        Connection& con = it->second;    // ★改动：原代码再次声明局部变量 Connection& con，
                                         //   现在直接用引用操作已找到的迭代器

        if (events & (EPOLLERR | EPOLLHUP)) {
            std::cout << "fd=" << client_fd << " 异常断开\n";
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

    /*
     * handle_read —— 读数据 + 批量解析 HTTP 请求 + 生成响应入队
     *
     * while 循环一次处理缓冲区中所有完整请求（支持 HTTP pipelining）。
     * 每个请求的响应以地址+长度的方式存入 resp_queue，零拷贝。
     */
    void handle_read(Connection& con, int client_fd) {
        char buf[READ_BUF_SIZE];
        // sizeof(buf) 不加 -1：append(buf, n) 使用显式长度，不需要空终止符
        ssize_t n = read(client_fd, buf, sizeof(buf));

        if (n > 0) {
            con.read_buf.append(buf, n);
            con.last_active = time(nullptr);   // 每轮读取只更新一次，不在循环内高频调用

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

                // 响应队列只存预编译字符串的地址+长度，零拷贝
                con.resp_queue.push_back(
                    should_close
                        ? Connection::ResponseRef{RESP_CLOSE, RESP_CLOSE_LEN}
                        : Connection::ResponseRef{RESP_KEEPALIVE, RESP_KEEPALIVE_LEN});
                has_response = true;

                con.read_buf.erase(0, header_end);
            }

            if (has_response) {
                std::cout << "[DEBUG] fd=" << client_fd << " queued " << con.resp_queue.size()
                          << " resp, switching to EPOLLOUT\n";
                if (!set_events(client_fd, EPOLLOUT)) {
                    std::cout << "[DEBUG] fd=" << client_fd << " set_events EPOLLOUT FAILED\n";
                }
            }
        }
        else if (n == 0) {
            std::cout << "fd=" << client_fd << " 断开连接\n";
            close_connection(client_fd);
        }
        else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::perror("read");
                close_connection(client_fd);
            }
        }
    }

    /*
     * handle_write —— 遍历响应队列，逐条发送到 socket
     *
     * 每条响应只存地址+长度 (ResponseRef)，无需拼接字符串。
     * 循环尽量一次 drain 整个队列，socket 缓冲区满时退出等待下次 EPOLLOUT。
     */
    void handle_write(Connection& con, int client_fd) {
        std::cout << "[DEBUG] fd=" << client_fd << " handle_write entry, queue="
                  << con.resp_queue.size() << " idx=" << con.resp_idx << "\n";
        while (con.resp_idx < con.resp_queue.size()) {
            auto& resp = con.resp_queue[con.resp_idx];
            ssize_t n;
            do {
                n = write(client_fd, resp.data + con.resp_off,
                          resp.len - con.resp_off);
            } while (n == -1 && errno == EINTR);

            std::cout << "[DEBUG] fd=" << client_fd << " write returned " << n
                      << " (errno=" << errno << ")\n";

            if (n > 0) {
                con.resp_off += n;
                if (con.resp_off == resp.len) {
                    con.resp_idx++;
                    con.resp_off = 0;
                } else {
                    return;  // 当前响应未发完，等待下次 EPOLLOUT
                }
            } else if (n == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::perror("write");
                    close_connection(client_fd);
                }
                return;
            } else {
                // n == 0：对端关闭连接
                std::cout << "fd=" << client_fd << " 写端关闭\n";
                close_connection(client_fd);
                return;
            }
        }

        // 队列中所有响应发送完毕
        if (!con.keep_alive) {
            std::cout << "响应发送完毕 fd=" << client_fd << " 关闭连接\n";
            close_connection(client_fd);
        } else {
            std::cout << "[DEBUG] fd=" << client_fd << " queue drained, switching to EPOLLIN\n";
            con.resp_queue.clear();
            con.resp_idx = 0;
            if (!set_events(client_fd, EPOLLIN)) {
                std::cout << "[DEBUG] fd=" << client_fd << " set_events EPOLLIN FAILED\n";
            }
        }
    }

    // 修改 epoll 监听事件，返回 true 表示成功
    bool set_events(int fd, uint32_t events) {
        struct epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            std::perror("epoll_ctl MOD");
            return false;
        }
        return true;
    }

    // =========================================================================
    //                           成员变量
    // =========================================================================
    /*
     * ★改动：原代码全部为全局变量：
     *          int epfd_global = -1;
     *          int listen_fd_global = -1;
     *          bool listen_paused = false;
     *          std::unordered_map<int,Connection> connections;
     *       现在全部移到类私有区域，外部无法直接访问。
     */
    int listen_fd_ = -1;
    int epfd_      = -1;
    int port_;

    /*
     * 【优化】删除了 listen_fd_global_ / epfd_global_ 成员变量（声明处）。
     *   原代码：
     *     int listen_fd_global_ = -1;  // 保留原全局变量语义
     *     int epfd_global_      = -1;
     *   已在 start() 中删除对应赋值代码，原因同上。
     */

    bool accept_paused_ = false;     // 原 listen_paused
    int  loop_cnt_      = 0;         // 原 main() 局部变量 loop_cnt

    std::unordered_map<int, Connection> connections_;  // 原全局 connections
};

// ============================================================================
//                                 main
// ============================================================================
/*
 * ★改动：原代码 main() 超过 110 行，包含全部初始化 + 事件循环 + 连接处理。
 *       现在 main() 只做三件事：创建服务器 → 启动 → 运行。4 行。
 *       这就是"工程化"的核心：把细节藏进合适的抽象层，让使用者一眼看懂流程。
 */
int main() {
    HttpServer server;
    if (!server.start()) return 1;
    server.run();
}
