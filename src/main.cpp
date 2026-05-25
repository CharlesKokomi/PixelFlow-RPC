#include <iostream>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <arpa/inet.h> 
#include <opencv2/opencv.hpp>
#include <errno.h>
#include "protocol.h"         
#include "message.pb.h"       
#include "processor.h"
#include "ThreadPool.h"
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#define MAX_EVENTS 1024
#define PORT 9000

// 将FD设为非阻塞
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 将FD恢复为阻塞
void setBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int main() {
    //线程池开个128，根据测试情况调整
    ThreadPool pool(128);
    ImageProcessor processor;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    //socket设置
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        return 1;
    }
    
    listen(listen_fd, 1024);
    setNonBlocking(listen_fd);

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::cout << "RPC Engine Started on Port " << PORT << "..." << std::endl;
    
    //开始主循环
    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; ++i) {
            int current_fd = events[i].data.fd;

            if (current_fd == listen_fd) {
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd < 0) continue;
                
                setNonBlocking(conn_fd);
                //边缘触发
                ev.events = EPOLLIN | EPOLLET; 
                ev.data.fd = conn_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev);
                std::cout << "[System] 新连接进入: FD " << conn_fd << std::endl;
            } else {
                int client_fd = current_fd;
                RpcHeader header;

                //先读包头
                int header_recv = 0;
                char* header_ptr = (char*)&header;
                bool read_failed = false;

                while (header_recv < (int)sizeof(RpcHeader)) {
                    int n = recv(client_fd, header_ptr + header_recv, sizeof(RpcHeader) - header_recv, 0);
                    if (n <= 0) {
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            //非阻塞模式下数据还没准备好，短等待后重试
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                            continue;
                        }
                        read_failed = true;
                        break;
                    }
                    header_recv += n;
                }

                //读出出错
                if (read_failed) {
                    std::cout << "[System] 客户端连接异常关闭 FD: " << client_fd << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    continue;
                }

                //字节序转换
                header.magic_number = ntohl(header.magic_number);
                header.version      = ntohl(header.version);
                header.body_len     = ntohl(header.body_len);
                header.type         = ntohl(header.type);
                
                //测试也加上魔术检验，有不干净的连接进来
                if (header.magic_number != 0xCAFEBABE) {
                    std::cerr << "[Protocol Error] 非法魔数！可能存在残留数据干扰，强制清理 FD: " << client_fd << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    continue;
                }
                //从epoll移除交给线程池
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                //阻塞状态恢复
                setBlocking(client_fd); 
                
                pool.enqueue([client_fd, header,&processor]() {
                    std::vector<char> body_buf(header.body_len);
                    //读Body放到这里让线程池管更合适
                    int total_recv = 0;
                    while (total_recv < (int)header.body_len) {
                        int r = recv(client_fd, body_buf.data() + total_recv, header.body_len - total_recv, 0);
                        if (r <= 0) { close(client_fd); return; }
                        total_recv += r;
                    }
                    std::cout << "[Thread " << std::this_thread::get_id() << "] 开始处理 FD: " << client_fd << std::endl;
                    
                    //开始处理
                    myrpc::ImageRequest req;
                    if (req.ParseFromArray(body_buf.data(), body_buf.size())) {
                        std::string processed_bytes;
                        if (processor.process(header.type, req.image_data(), processed_bytes)) {
                            
                            myrpc::ImageResponse resp;
                            resp.set_processed_data(processed_bytes);
                            resp.set_success(true);

                            std::string resp_str;
                            resp.SerializeToString(&resp_str);

                            RpcHeader resp_header;
                            resp_header.magic_number = htonl(0xCAFEBABE);
                            resp_header.version      = htonl(1);
                            resp_header.body_len     = htonl((uint32_t)resp_str.size());
                            resp_header.type         = htonl(header.type);

                            // Worker线程负责写回响应
                            send(client_fd, &resp_header, sizeof(RpcHeader), 0);
                            send(client_fd, resp_str.data(), resp_str.size(), 0);
                        }
                    }
                    // 处理完毕，Worker线程关闭连接
                    close(client_fd);
                    std::cout << "[Thread " << std::this_thread::get_id() << "] 处理完毕并关闭 FD: " << client_fd << std::endl;
                });
            }
        }
    }
    return 0;
}