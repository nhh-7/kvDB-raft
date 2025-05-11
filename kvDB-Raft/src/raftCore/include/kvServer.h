#pragma once

#include <boost/any.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/foreach.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <memory>
#include "kvServerRPC.pb.h"
#include "raft.h"
#include "skipList.h"

class KvServer : raftKVRpcProctoc::kvServerRpc
{
private:
    std::mutex m_mtx;
    int m_me; // 节点编号
    std::shared_ptr<Raft> m_raftNode; // 当前节点raft实例
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // kvServer和raft节点的通信管道
    int m_maxRaftState; // 触发快照的最大状态大小阈值

    std::string m_serializedKVData; // 暂存序列化后的kvDB快照数据（中间变量）
    SkipList<std::string, std::string> m_skipList;
    // std::unordered_map<std::string, std::string> m_kvDB;

    std::unordered_map<int, LockQueue<Op> *> waitApplyCh; // Raft日志Index到等待操作的队列映射

    std::unordered_map<std::string, int> m_lastRequestId; // 每个Client最近成功执行的RequestId

    int m_lastSnapshotRaftLogIndex; // 上一次制作快照时Raft日志Index

public:
    KvServer() = delete;
    KvServer(int me, int maxraftstate, std::string nodeInfoFileName, short port);
    void StartKVServer();
    void DprintfKVDB();

    void ExecuteAppendOpOnKVDB(Op op);
    void ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist);
    void ExecutePutOpOnKVDB(Op op);

    void Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply);

    void GetCommandFromRaft(ApplyMsg message);
    bool ifRequestDuplicate(std::string ClientId, int RequestId);
    
    void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    // 持续监听 applyChan，一旦 Raft 日志应用完成，就从中取出 ApplyMsg 并分发给状态机执行
    void ReadRaftApplyCommandLoop();

    void ReadSnapShotToInstall(std::string snapshot);

    bool SendMessageToWaitChan(const Op &op, int raftIndex);

    // 检查是否需要制作快照，需要的话就向raft之下制作快照
    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion);

    // Handler the SnapShot from kv.rf.applyCh
    void GetSnapShotFromRaft(ApplyMsg message);

    std::string MakeSnapShot();

public: // for rpc
    void PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                   ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) override;

    void Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
             ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) override;

private:
    friend boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & m_serializedKVData;
        ar & m_lastRequestId;
    }

    std::string getSnapshotData()
    {
        m_serializedKVData = m_skipList.dump_file();
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        m_serializedKVData.clear();
        return ss.str();
    }

    void parseFromString(const std::string &str)
    {
        std::stringstream ss(str);
        boost::archive::text_iarchive ia(ss);
        ia >> *this;
        m_skipList.load_file(m_serializedKVData);
        m_serializedKVData.clear();
    }
};