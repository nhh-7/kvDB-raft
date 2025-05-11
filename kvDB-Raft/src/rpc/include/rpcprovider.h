#pragma once

#include <google/protobuf/descriptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <google/protobuf/service.h>

/** 负责
 *   发布本地服务（NotifyService）
     启动网络监听（Run）
     接收客户端请求，解析协议，调用对应服务方法（OnMessage）
     返回结果给客户端（SendRpcResponse）
 */

// 框架提供的专门发布rpc服务的网络对象类
class RpcProvider
{
public:
    // 这里是框架提供给外部使用的，可以发布rpc方法的函数接口
    // 注册服务到rpc框架，让框架知道你有哪些功能可以被远程调用。
    void NotifyService(google::protobuf::Service *service);
    // 启动rpc服务节点，开始提供rpc远程网络调用服务
    void Run(int nodeIndex, short port);

private:
    muduo::net::EventLoop m_eventLoop;
    std::shared_ptr<muduo::net::TcpServer> m_muduo_server;

    // service服务类型信息
    struct ServiceInfo
    {
        google::protobuf::Service *m_service;                                                    // 指向具体的服务对象（可以调用里面的方法）
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor *> m_methodMap; // 保存服务方法
    };
    // 存储注册成功的服务对象和其服务方法的所有信息
    std::unordered_map<std::string, ServiceInfo> m_serviceMap; // 收到远程请求时，能通过名字找到对应的服务和方法。

    // 新的socket连接回调  每有新的 TCP 连接建立或者断开，都会触发这里。
    void OnConnection(const muduo::net::TcpConnectionPtr &);
    // 已建立连接用户的读写事件回调 真正实现"调用远程方法"的地方
    void OnMessage(const muduo::net::TcpConnectionPtr &, muduo::net::Buffer *, muduo::Timestamp);
    // Closure的回调操作，用于序列化rpc的响应和网络发送
    void SendRpcResponse(const muduo::net::TcpConnectionPtr &, google::protobuf::Message *);

public:
    ~RpcProvider();
};