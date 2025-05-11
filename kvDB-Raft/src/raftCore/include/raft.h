#pragma once
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "ApplyMsg.h"
#include "Persister.h"
#include "boost/any.hpp"
#include "boost/serialization/serialization.hpp"
#include "config.h"
#include "monsoon.h"
#include "raftRpcUtil.h"
#include "util.h"

constexpr int Disconnected = 0;
constexpr int AppNormal = 1;

// 投票状态
constexpr int killed = 0;
constexpr int Voted = 1;  // 本轮已经投过票
constexpr int Expire = 2; // 投票过期
constexpr int Normal = 3;

class Persister;

class Raft : public raftRpcProctoc::raftRpc
{
private:
    std::mutex m_mtx;
    // 集群中所有节点的 RPC Stub（RPC 客户端对象数组）
    std::vector<std::shared_ptr<RaftRpcUtil>> m_peers;
    std::shared_ptr<Persister> m_persister;
    int m_me;

    // 当前任期号。
    int m_currentTerm;
    // 本任期投票给了谁。
    int m_votedFor;
    std::vector<raftRpcProctoc::LogEntry> m_logs; // 日志条目数组，包含了状态机要执行的指令集，以及收到领导时的任期号(不包含已经被快照了的内容)  是一个从 m_lastSnapshotIncludeIndex + 1 开始的连续日志序列

    // 已知被大多数节点复制的最大日志索引 决定了哪些日志可以交付给上层应用
    int m_commitIndex;
    int m_lastApplied; // 已经汇报给状态机（上层应用）的log 的index

    // 这两个状态的下标1开始，因为通常commitIndex和lastApplied从0开始，应该是一个无效的index，因此下标从1开始
    // Leader特有
    std::vector<int> m_nextIndex;  // m_nextIndex[i]：对每个 follower，下一条应该发送的日志索引。
    std::vector<int> m_matchIndex; // m_matchIndex[i]：对每个 follower，已经确认复制的最大日志索引。
    enum Status
    {
        Follower,
        Candidate,
        Leader
    };
    Status m_status; // 身份

    // Raft → 应用层（比如 KVServer）通信的管道
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan;

    // 选举超时 上一次心跳时间
    std::chrono::_V2::system_clock::time_point m_lastResetElectionTime;
    // 心跳超时，用于leader
    std::chrono::_V2::system_clock::time_point m_lastResetHeartBeatTime;

    // 储存了快照中的最后一个日志的Index和Term
    int m_lastSnapshotIncludeIndex;
    int m_lastSnapshotIncludeTerm;

    // 协程
    std::unique_ptr<monsoon::IOManager> m_ioManager = nullptr;

public:
    void AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply);

    // 返回当前任期和是否是 Leader。
    void GetState(int *term, bool *isLeader);

    /**
     * 选举整体流程：
        election timeout 超时，变 Candidate。
        任期 +1，给所有 peers 发 RequestVote。
        收到大多数票 → 成为 Leader。
        收到更大 term 的 reply → 回到 Follower。
     */
    // 检测是否 election timeout，如果超时就从Follower切换到 Candidate
    void electionTimeOutTicker();
    // 主动发起一次选举（Candidate角色）
    void doElection();
    // 给某个 server 发送 RequestVote RPC
    bool sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                         std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum);
    void RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply);

    /**
     * 日志复制（心跳）
     */
    // 发起心跳，只有leader才需要发起心跳
    void doHeartBeat();
    // 后台循环发送心跳
    void leaderHeartBeatTicker();
    // 发 AppendEntries RPC。
    bool sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                           std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums);
    // 根据 matchIndex 更新 commitIndex
    void leaderUpdateCommitIndex();

    /**
     * 日志操作工具
     */
    // 获取当前 follower 应该接收的前一个日志位置
    void getPrevLogInfo(int server, int *preIndex, int *preTerm);
    bool matchLog(int logIndex, int logTerm);
    bool UpToDate(int index, int term);
    int getLastLogIndex();
    int getLastLogTerm();
    void getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm);
    int getLogTermFromLogIndex(int logIndex);
    // 根据全局日志索引（LogIndex）转换为 对应于m_logs 中的下标。
    int getSlicesIndexFromLogIndex(int logIndex);

    /**
     * 状态持久化
     */
    // 保存当前 term、votedFor、日志、快照信息
    void persist();
    // 从 Persister 中恢复持久化数据
    void readPersist(std::string data);
    // 返回序列化好的数据
    std::string persistData();

    /**
     *  Leader 处理客户端新命令的接口
     */
    void Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);

    /**
     * 快照相关
     */
    // index代表是快照apply应用的index,而snapshot代表的是上层service传来的快照字节流，包括了Index之前的数据
    // 这个函数的目的是把安装到快照里的日志抛弃，并安装快照数据，同时更新快照下标，属于peers自身主动更新，与leader发送快照不冲突
    // 即服务层主动发起请求raft保存snapshot里面的数据，index是用来表示snapshot快照执行到了哪条命令 上层应用通知 Raft 可以制作快照，裁剪日志
    void Snapshot(int index, std::string snapshot);
    // Follower 收到 Leader 发送的快照后，安装快照
    bool CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot);
    void InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                         raftRpcProctoc::InstallSnapshotResponse *reply);
    // Leader 主动推送快照给落后的节点。
    void leaderSendSnapShot(int server);

    /**
     * 应用日志到状态机
     */
    // 后台协程，不断检查可以 apply 的日志
    void applierTicker();
    // 把 ApplyMsg 推送到应用层（KVServer）
    void pushMsgToKvServer(ApplyMsg msg);
    // 批量读取应用日志
    std::vector<ApplyMsg> getApplyLogs();

    int getNewCommandIndex();

    int GetRaftStateSize();

public:
    // 重写基类方法,因为rpc远程调用真正调用的是这个方法
    // 序列化，反序列化等操作rpc框架都已经做完了，因此这里只需要获取值然后真正调用本地方法即可。
    void AppendEntries(google::protobuf::RpcController *controller, const ::raftRpcProctoc::AppendEntriesArgs *request,
                       ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done) override;
    void InstallSnapshot(google::protobuf::RpcController *controller,
                         const ::raftRpcProctoc::InstallSnapshotRequest *request,
                         ::raftRpcProctoc::InstallSnapshotResponse *response, ::google::protobuf::Closure *done) override;
    void RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                     ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done) override;

public:
    void init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
              std::shared_ptr<LockQueue<ApplyMsg>> applyCh);

private:
    // for persist
    class BoostPersistRaftNode
    {
    public:
        friend class boost::serialization::access;
        // When the class Archive corresponds to an output archive, the
        // & operator is defined similar to <<.  Likewise, when the class Archive
        // is a type of input archive the & operator is defined similar to >>.
        template <class Archive>
        void serialize(Archive &ar, const unsigned int version)
        {
            ar & m_currentTerm;
            ar & m_votedFor;
            ar & m_lastSnapshotIncludeIndex;
            ar & m_lastSnapshotIncludeTerm;
            ar & m_logs;
        }
        int m_currentTerm;
        int m_votedFor;
        int m_lastSnapshotIncludeIndex;
        int m_lastSnapshotIncludeTerm;
        std::vector<std::string> m_logs;
        std::unordered_map<std::string, int> umap;
    };
};
