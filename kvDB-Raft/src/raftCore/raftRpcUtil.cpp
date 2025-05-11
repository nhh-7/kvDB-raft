#include "./include/raftRpcUtil.h"

#include "../rpc/include/mprpcchannel.h"
#include "../rpc/include/mprpccontroller.h"

RaftRpcUtil::RaftRpcUtil(std::string ip, short port)
{
    stub_ = new raftRpcProctoc::raftRpc_Stub(new MprpcChannel(ip, port, true));
}

RaftRpcUtil::~RaftRpcUtil()
{
    delete stub_;
}

bool RaftRpcUtil::AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response)
{
    MprpcController controller;
    stub_->AppendEntries(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response)
{
    MprpcController controller;
    stub_->InstallSnapshot(&controller, args, response, nullptr);
    return !controller.Failed();
}

bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response)
{
    MprpcController controller;
    stub_->RequestVote(&controller, args, response, nullptr);
    return !controller.Failed();
}