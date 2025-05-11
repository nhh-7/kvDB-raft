#include "./include/mprpcchannel.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include "./include/mprpccontroller.h"
#include "./include/rpcheader.pb.h"
#include "../common/include/util.h"

/**
     *  任务
     *  组织RPC请求数据（序列化 request）
        网络发送（发送到服务端）
        等待响应并解析（填充 response）
        method：被调用的远程方法的描述信息（比如服务名、方法名等）

     */
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor *method,
                              google::protobuf::RpcController *controller, const google::protobuf::Message *request,
                              google::protobuf::Message *response, google::protobuf::Closure *done)
{
    // 如果连接断了，重新连接。
    if (m_clientFd == -1)
    {
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt)
        {
            DPrintf("[func-MprpcChannel::CallMethod]重连接ip：{%s} port{%d}失败", m_ip.c_str(), m_port);
            controller->SetFailed(errMsg);
            return;
        }
        else
        {
            DPrintf("[func-MprpcChannel::CallMethod]连接ip：{%s} port{%d}成功", m_ip.c_str(), m_port);
        }
    }

    const google::protobuf::ServiceDescriptor *sd = method->service();
    std::string service_name = sd->name();
    std::string method_name = method->name();

    // 获取参数的序列化字符串长度 args_size
    uint32_t args_size{};
    std::string args_str;
    if (request->SerializeToString(&args_str))
    {
        args_size = args_str.size();
    }
    else
    {
        controller->SetFailed("serialize request error");
        return;
    }

    RPC::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size(args_size);

    std::string rpc_header_str;
    if (!rpcHeader.SerializeToString(&rpc_header_str))
    {
        controller->SetFailed("serialize rpc eh ader error");
        return;
    }

    // 使用protobuf的CodedOutputStream来构建发送的数据流
    std::string send_rpc_str; // 用来存储最终发送的数据
    {
        // 创建一个StringOutputStream用于写入send_rpc_str
        google::protobuf::io::StringOutputStream string_output(&send_rpc_str);
        google::protobuf::io::CodedOutputStream coded_output(&string_output);

        // 先写入header的长度（变长编码）
        coded_output.WriteVarint32(static_cast<uint32_t>(rpc_header_str.size()));

        coded_output.WriteString(rpc_header_str);
    }
    // 最后，将请求参数附加到send_rpc_str后面
    send_rpc_str += args_str;

    // 发送rpc请求
    // 失败会重试连接再发送，重试连接失败会直接return
    while (send(m_clientFd, send_rpc_str.c_str(), send_rpc_str.size(), 0) == -1)
    {
        char errtxt[512] = {0};
        sprintf(errtxt, "send error errno: %d", errno);
        std::cout << "尝试重新连接，对方ip：" << m_ip << " 对方端口" << m_port << std::endl;
        close(m_clientFd);
        m_clientFd = -1;
        std::string errMsg;
        bool rt = newConnect(m_ip.c_str(), m_port, &errMsg);
        if (!rt)
        {
            controller->SetFailed(errMsg);
            return;
        }
    }

    // 接收响应数据
    char recv_buf[1024] = {0};
    int recv_size = 0;
    if ((recv_size = recv(m_clientFd, recv_buf, 1024, 0)) == -1)
    {
        close(m_clientFd);
        m_clientFd = -1;
        char errtxt[512] = {0};
        sprintf(errtxt, "recv error! errno:%d", errno);
        controller->SetFailed(errtxt);
        return;
    }

    // 反序列化rpc调用的响应数据
    // response 是调用者准备好的空对象
    if (!response->ParseFromArray(recv_buf, recv_size))
    {
        char errtxt[1050] = {0};
        sprintf(errtxt, "parse error! response_str:%s", recv_buf);
        controller->SetFailed(errtxt);
        return;
    }
}

bool MprpcChannel::newConnect(const char *ip, uint16_t port, std::string *errMsg)
{
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientFd)
    {
        char errtxt[512] = {0};
        sprintf(errtxt, "create socket error! errno:%d", errno);
        m_clientFd = -1;
        *errMsg = errtxt;
        return false;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    // 连接rpc服务节点
    if (connect(clientFd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        close(clientFd);
        char errtxt[512] = {0};
        sprintf(errtxt, "connect fail! errno:%d", errno);
        m_clientFd = -1;
        *errMsg = errtxt;
        return false;
    }
    m_clientFd = clientFd;
    return true;
}

MprpcChannel::MprpcChannel(std::string ip, short port, bool connectNow) : m_ip(ip), m_port(port), m_clientFd(-1)
{
    // 读取配置文件rpcserver的信息
    // std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    // uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());
    // rpc调用方想调用service_name的method_name服务，需要查询zk上该服务所在的host信息
    if (!connectNow)
    {
        return;
    }
    std::string errMsg;
    auto rt = newConnect(ip.c_str(), port, &errMsg);
    int tryCount = 3;
    while (!rt && tryCount--)
    {
        std::cout << errMsg << std::endl;
        rt = newConnect(ip.c_str(), port, &errMsg);
    }
}