#pragma once

#include <iostream>
#include "../../raftRpcPro/include/kvServerRPC.pb.h"
#include "../../rpc/include/mprpcchannel.h"
#include "../../rpc/include/mprpccontroller.h"
#include "../../rpc/include/rpcprovider.h"

/// @brief 维护当前节点对其他某一个结点的所有rpc通信，包括接收其他节点的rpc和发送
// 对于一个节点来说，对于任意其他的节点都要维护一个rpc连接，
class raftServerRpcUtil
{
public:
    bool Get(raftKVRpcProctoc::GetArgs *GetArgs, raftKVRpcProctoc::GetReply *reply);
    bool PutAppend(raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    raftServerRpcUtil(std::string ip, short port);
    ~raftServerRpcUtil();

private:
    // stub 是通过 Protobuf 自动生成的远程调用代理对象。
    raftKVRpcProctoc::kvServerRpc_Stub *stub;
};