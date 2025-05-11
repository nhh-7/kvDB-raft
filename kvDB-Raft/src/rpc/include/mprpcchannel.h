#pragma once

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <algorithm> // 包含 std::generate_n() 和 std::generate() 函数的头文件
#include <functional>
#include <iostream>
#include <map>
#include <random> // 包含 std::uniform_int_distribution 类型的头文件
#include <unordered_map>
#include <vector>

// 真正负责发送和接受的前后处理工作
//  如消息的组织方式，向哪个节点发送等等
// 用于发送 RPC 请求（重写了 google::protobuf::RpcChannel 的 CallMethod）
class MprpcChannel : public google::protobuf::RpcChannel // RpcChannel 是 protobuf 官方定义的一个抽象基类，它规定了 RPC 框架应该实现的接口，主要是 CallMethod
{
public:
    // 所有通过stub代理对象调用的rpc方法，都走到这里了
    /**
     *  任务
     *  组织RPC请求数据（序列化 request）
        网络发送（发送到服务端）
        等待响应并解析（填充 response）
        method：被调用的远程方法的描述信息（比如服务名、方法名等）
        controller：用于设置/获取 RPC 的一些控制参数（如超时、错误信息等）
     */
    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                    const google::protobuf::Message *request, google::protobuf::Message *response,
                    google::protobuf::Closure *done) override;
    MprpcChannel(std::string ip, short port, bool connectNow);

private:
    int m_clientFd;
    const std::string m_ip;
    const uint16_t m_port;

    /// @brief 连接ip和端口,并设置m_clientFd
    /// @param ip ip地址，本机字节序
    /// @param port 端口，本机字节序
    /// @return 成功返回空字符串，否则返回失败信息
    bool newConnect(const char *ip, uint16_t port, std::string *errMsg);
};