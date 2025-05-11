#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include "raftServerRpcUtil.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>
#include "../../raftRpcPro/include/kvServerRPC.pb.h"
#include "../../rpc/include/mprpcconfig.h"

/**
 * Clerk 就是 Raft KV 服务的客户端抽象，负责帮应用发送 Get/Put/Append 请求到 Raft 集群。
    可以理解成：
    它代表了客户端视角。
    帮用户操作分布式 Key-Value 服务。
    自动管理连接、请求、重试、leader 跟踪等细节。
 *
 */

class Clerk
{
public:
    Clerk();

public:
    void Init(std::string configFileName); // 连接到集群
    std::string Get(std::string key);      // 从 Raft 集群读取一个键对应的值

    void Put(std::string key, std::string value);    // 向 Raft 集群写入一个键值对。
    void Append(std::string key, std::string value); // 在原有值后追加内容。

private:
    std::vector<std::shared_ptr<raftServerRpcUtil>> m_servers; // 保存与所有 Raft 节点的连接
    std::string m_clientId;
    int m_requestId;      // 	单调递增的请求号
    int m_recentLeaderId; // 	可能是 Leader 的节点编号

    std::string Uuid()
    {
        return std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand()) + std::to_string(rand());
        // 用于返回随机的clientId
    }

    void PutAppend(std::string key, std::string value, std::string op);
};
