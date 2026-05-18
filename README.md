从零手搓的 C++ HTTP 服务器

## 功能

- 基于 Linux socket API 的 TCP 服务器
- HTTP/1.0 请求解析与路由
- 静态 HTML 页面返回
- 404 错误处理
- JSON API 端点，使用自研 JSON 库动态生成响应
- 连接关闭

## 编译与运行

```bash
g++ -std=c++11 myhttpserver.cpp cppjson.cpp -o myhttpserver -pthread
./myhttpserver
```

## 测试

```bash
curl -i http://localhost:8080/
curl -i http://localhost:8080/api/hello
curl -i http://localhost:8080/notexist
```

## 项目结构

```
.
├── myhttpserver.cpp   # 服务器主程序
├── cppjson.h          # JSON 库头文件
├── cppjson.cpp        # JSON 库实现
└── README.md
```

## 自研 JSON 库

支持 object、array、string、number、boolean、null 六种类型。递归下降解析，支持嵌套，序列化输出到任意 std::ostream。

详情见https://github.com/keshao0221/cppjson-editor

## 版本路线

- V0（已完成）：单线程阻塞 I/O，基础路由，JSON API
- V1（计划中）：epoll + 非阻塞 I/O + Reactor 模式
- V2（后续）：线程池 + 内存池 + 定时器

## 许可证

MIT License
```
