// =================================================================
// 高性能 Reactor 服务器开发必备头文件清单
// =================================================================

// 1. 网络编程核心
#include <sys/socket.h> // socket, bind, listen, accept, send, recv
#include <netinet/in.h> // sockaddr_in, IPPROTO_TCP
#include <arpa/inet.h>  // inet_addr, htons, ntohl 等地址转换
#include <netdb.h>      // gethostbyname 等域名解析

// 2. Linux 系统调用与 IO 复用
#include <sys/epoll.h>   // epoll_create, epoll_ctl, epoll_wait
#include <sys/types.h>   // 基本系统数据类型
#include <sys/eventfd.h> // eventfd (跨线程通知神器)
#include <unistd.h>      // close, read, write, fork, pipe
#include <fcntl.h>       // fcntl, O_NONBLOCK (设置非阻塞)
#include <signal.h>      // signal (处理 SIGPIPE 等信号)

// 3. 错误处理与调试
#include <errno.h>  // errno, EAGAIN, EWOULDBLOCK
#include <stdio.h>  // printf, perror
#include <string.h> // memset, bzero, strerror

// 4. C++ 11/14/17 标准库 (线程与并发)
#include <iostream>           // 标准输入输出
#include <string>             // std::string
#include <vector>             // 容器
#include <queue>              // 任务队列
#include <unordered_map>      // 哈希表 (fd 与 Connection 映射)
#include <memory>             // std::shared_ptr, std::unique_ptr
#include <functional>         // std::function, std::bind
#include <thread>             // std::thread
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable (同步等待)
#include <atomic>             // std::atomic (原子操作)

// 5. 时间与实用工具
#include <chrono>    // 时间处理 (超时控制)
#include <algorithm> // std::min, std::max

#include <sstream>
#include <memory>
#include <unordered_map>
#include <set>

// =================================================================
using namespace std;
class HttpConnection : public enable_shared_from_this<HttpConnection>
{
public:
    int epfd; // 记录被哪个epoll实例监控
    int fd;   // 记录用户套接字编号
    string readBuffer;
    string WriteBuffer;
    mutex _mtx;

public:
    HttpConnection(int fd, int epfd) : fd(fd), epfd(epfd) {}
    int getFd()
    {
        return fd;
    }
    void add(char *buf, int len)
    {
        readBuffer.append(buf, len);
    }
    int isComplete()
    {
        size_t header_end = readBuffer.find("\r\n\r\n");
        if (header_end == string::npos)
        {
            return 0;
        }
        size_t pos = readBuffer.find("Content-Length: ");
        if (pos != string::npos && pos < header_end)
        {
            size_t val_start = pos + 16;
            size_t val_end = readBuffer.find("\r\n", val_start);
            int content_len = stoi(readBuffer.substr(val_start, val_end - val_start));
            size_t current_body_len = readBuffer.size() - (header_end + 4);
            if (current_body_len >= (size_t)content_len)
            {
                return content_len + header_end + 4;
            }
            else
            {
                return 0;
            }
        }
        return header_end + 4;
    }
    string popPackage(int total_len)
    {
        string package = readBuffer.substr(0, total_len);
        readBuffer.erase(0, total_len);
        return package;
    }
};
class ThreadPool
{
    queue<pair<string, shared_ptr<HttpConnection>>> messages;
    queue<thread> pq;
    condition_variable cv;
    mutex _mtx;

public:
    void init(int threadNum)
    {
        for (int i = 0; i < threadNum; i++)
        {
            thread newThread(&ThreadPool::work, this);
            pq.push(move(newThread));
        }
    }
    void push(string message, shared_ptr<HttpConnection> ptr)
    {
        unique_lock<mutex> myLock(_mtx);
        messages.push({message, ptr});
        cv.notify_all();
    }
    string parse(string &readBuffer)
    {
        stringstream ss(readBuffer);
        string method, path, version;

        // 1. 解析第一行：[Method] [Path] [Version]
        if (!(ss >> method >> path >> version))
        {
            return ""; // 解析失败，可能是残缺报文
        }

        string content;                            // 存放网页 Body 内容
        string statusLine = "HTTP/1.1 200 OK\r\n"; // 默认状态码

        // 2. 逻辑分发（模拟简单路由）
        if (method == "GET")
        {
            if (path == "/" || path == "/index.html")
            {
                content = "<h1>Home</h1><p>Reactor 服务器主页已送达！</p>";
            }
            else if (path == "/art")
            {
                content = "<h1>Art Gallery</h1><p>这里放一些内容 (Base64 or Image path)。</p>";
            }
            else
            {
                statusLine = "HTTP/1.1 404 Not Found\r\n";
                content = "<h1>404</h1><p>没找到这个页面，多半是还没摸出来。</p>";
            }
        }
        else if (method == "POST" || method == "PUT" || method == "PATCH")
        {
            // 简单回复，实际需要解析 Content-Length 后的 Body
            content = "<h1>Success</h1><p>收到了你的 " + method + " 请求。</p>";
        }
        else if (method == "HEAD")
        {
            // HEAD 只需要头，不给 content，所以这里保持 content 为空即可
        }
        else
        {
            statusLine = "HTTP/1.1 405 Method Not Allowed\r\n";
            content = "<h1>405</h1><p>暂不支持该请求方法。</p>";
        }

        // 3. 封装响应报文（必须严格遵守 HTTP 格式）
        string WriteBuffer;
        WriteBuffer = statusLine;
        WriteBuffer += "Content-Type: text/html; charset=utf-8\r\n";
        WriteBuffer += "Content-Length: " + to_string(content.size()) + "\r\n";
        WriteBuffer += "Connection: keep-alive\r\n";
        WriteBuffer += "\r\n"; // 关键：Header 和 Body 之间的空行

        if (method != "HEAD")
        { // HEAD 方法不发送 body
            WriteBuffer += content;
        }
        return WriteBuffer;
    }
    void work()
    {
        while (1)
        {
            unique_lock<mutex> connLock(_mtx);
            while (messages.empty())
            {
                cv.wait(connLock);
            }
            auto [msg, ptr] = messages.front();
            messages.pop();
            string reply = parse(msg);
            unique_lock<mutex> myLock(ptr->_mtx);
            ptr->WriteBuffer += reply;
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.ptr = ptr.get();
            epoll_ctl(ptr->epfd, EPOLL_CTL_MOD, ptr->fd, &ev); // 改成写触发，这样可以触发可写
        }
    }
};
class SubReactor
{
public:
    queue<int> pq;
    int epfd; // epoll实例
    int efd;
    thread m_thread;
    mutex _mtx;
    ThreadPool *threadPool;

public:
    void init(ThreadPool *trdPool)
    {
        threadPool = trdPool;
        efd = eventfd(0, EFD_NONBLOCK);
        // 首先要把efd注册到监听表里
        epfd = epoll_create(1);
        struct epoll_event ev;
        ev.data.ptr = nullptr;
        ev.events = EPOLLIN;
        epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);
        m_thread = thread(&SubReactor::receive, this);
        m_thread.detach();
    }
    // 因为锁是该类持有，只能由该类来执行push操作
    void push(int clntSock)
    {
        unique_lock<mutex> myLock(_mtx);
        pq.push(clntSock);
    }
    void receive()
    {
        struct epoll_event *events = new epoll_event[100];
        while (1)
        {
            int num = epoll_wait(epfd, events, 100, -1);
            for (int i = 0; i < num; i++)
            {
                uint32_t revents = events[i].events; // 看下触发方式
                // 如果是来了新客人
                if (events[i].data.ptr == nullptr)
                {
                    char buf[10];
                    read(efd, buf, sizeof(buf));
                    unique_lock<mutex> myLock(_mtx);
                    while (!pq.empty())
                    {
                        auto clntSock = pq.front();
                        HttpConnection *ptr = new HttpConnection(clntSock, epfd);
                        struct epoll_event ev;
                        ev.events = EPOLLIN;
                        ev.data.ptr = ptr;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, clntSock, &ev);
                        pq.pop();
                    }
                }
                else
                {
                    int aliveFlag = 1; // 用户是否切断连接
                    // 如果老客户发来消息
                    if (revents & EPOLLIN)
                    {
                        HttpConnection *rawptr = (HttpConnection *)events[i].data.ptr; // 用户的ptr里边存了一条不完整的消息
                        auto ptr = rawptr->shared_from_this();
                        int fd = ptr->getFd();
                        while (1)
                        {
                            char buf[1024];
                            int bytes = read(fd, buf, sizeof(buf));
                            if (bytes > 0)
                            {
                                ptr->add(buf, bytes);
                                int total_len = ptr->isComplete();
                                while (total_len)
                                {
                                    // 截取
                                    string content = ptr->popPackage(total_len);
                                    // 调用工作线程处理
                                    threadPool->push(content, ptr);
                                    //
                                    total_len = ptr->isComplete();
                                }
                            }
                            else
                            {
                                if (bytes == 0) // 客户切断连接
                                {
                                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                                    close(fd);
                                    st.erase(ptr);
                                    aliveFlag = 0;
                                    break;
                                }
                                else
                                { // 消息读完了
                                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                                        break;
                                }
                            }
                        }
                    }
                    // 如果是可写触发，说明：1.工作线程写完了改状态，2.缓冲区有空间接收
                    if ((revents & EPOLLOUT) && aliveFlag)
                    {

                        HttpConnection *ptr = (HttpConnection *)events[i].data.ptr;
                        unique_lock<mutex> myLock(ptr->_mtx);
                        string reply = ptr->WriteBuffer;
                        size_t len = send(ptr->fd, reply.c_str(), reply.size(), 0);
                        if (len > 0)
                        {
                            ptr->WriteBuffer.erase(0, len);
                            if (ptr->WriteBuffer.empty())
                            {
                                // 全发完了，大功告成，修改 epoll 移除 EPOLLOUT，避免“忙等待”
                                struct epoll_event ev;
                                ev.events = EPOLLIN;
                                ev.data.ptr = ptr;
                                epoll_ctl(ptr->epfd, EPOLL_CTL_MOD, ptr->fd, &ev);
                            }
                        }
                    }
                }
            }
        }
    }
};

class SubReactorPool
{
public:
    vector<SubReactor *> vec; // 开四个线程
    void init(ThreadPool *trdPool)
    {
        for (int i = 0; i < 4; i++)
        {
            auto ptr = new SubReactor();
            ptr->init(trdPool);
            vec.emplace_back(ptr);
        }
    }
};
int main()
{
    signal(SIGPIPE, SIG_IGN);
    ThreadPool *threadPool = new ThreadPool();
    threadPool->init(4);
    SubReactorPool *subReactorPool = new SubReactorPool();
    subReactorPool->init(threadPool);
    int epfd = epoll_create(1);
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8888);
    socklen_t len = sizeof(addr);
    int opt = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listenSocket, (sockaddr *)&addr, len);
    listen(listenSocket, 1024);
    struct epoll_event ev;
    ev.data.fd = listenSocket;
    ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listenSocket, &ev);
    struct epoll_event *events = new epoll_event[100];
    while (1)
    {
        static int count = 0;
        epoll_wait(epfd, events, 100, -1);
        while (true)
        {
            sockaddr_in addr;
            socklen_t len = sizeof(addr);
            int clntSock = accept(listenSocket, (sockaddr *)&addr, &len);
            if (clntSock == -1)
            {
                break;
            }
            int flags = fcntl(clntSock, F_GETFL, 0);
            fcntl(clntSock, F_SETFL, flags | O_NONBLOCK);
            int reactorNum = count % 4;
            count++;
            auto Reactor = (subReactorPool->vec)[reactorNum];
            Reactor->push(clntSock);
            uint64_t wakeup_signal = 1;
            write(Reactor->efd, &wakeup_signal, sizeof(wakeup_signal));
        }
    }
}