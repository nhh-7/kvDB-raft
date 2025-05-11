#pragma once

#include "../../raftRpcPro/include/raftRPC.pb.h"

/**
 * 与raftServerRpcUtil是一对的
 * RaftRpcUtil用于 Raft 节点间进行复制日志、选举
 * raftServerRpcUtil用于处理客户端键值请求
 */

/// @brief 维护当前节点对其他某一个结点的所有rpc发送通信的功能
// 对于一个raft节点来说，对于任意其他的节点都要维护一个rpc连接，即MprpcChannel
// Raft 协议内部节点对节点的通信辅助类，封装了 RPC 通信细节。
class RaftRpcUtil
{
public:
    // 建立一条到远端节点的 RPC 通道
    RaftRpcUtil(std::string ip, short port);
    ~RaftRpcUtil();

    /**
     * 给指定远程节点发一次 AppendEntries 请求。用于心跳或日志同步
     * args 包含当前节点的日志、term、leaderId等重要信息
     * response 用于接收对方返回的结果（term、success标记）
     */
    bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
    /**
     * 用于Follower 落后太多，日志太大，不好补发时直接发整个快照安装
     */
    bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
    /**
     * 用于：节点进入 Candidate 状态，开始新一轮选举，向其他节点拉票
     * 向远程节点请求投票。
     * args 里包括 candidate 的 term、日志索引、日志 term 等。
     * response 里返回是否同意投票。
     */
    bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

private:
    raftRpcProctoc::raftRpc_Stub *stub_;
};