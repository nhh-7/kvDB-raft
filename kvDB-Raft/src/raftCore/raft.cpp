#include "./include/raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>
#include "../common/include/config.h"
#include "../common/include/util.h"

/**
 * 日志索引 index：日志条目的物理位置编号（从 1 开始连续递增，不随 term 改变）。
   日志任期 term：日志条目被哪个 term 的 leader 创建的，表示领导者任期。
 * 同一个 term 下，index 越大，日志越新。
 */

void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh)
{
    m_peers = peers; // Raft 集群中所有节点的RPC通信工具对象
    m_me = me;       // 标记自己，不能给自己发送rpc
    m_persister = persister;
    m_mtx.lock();
    this->applyChan = applyCh; // 与kvServer沟通
    m_currentTerm = 0;
    m_status = Follower; // 初始化身份为Follower
    m_commitIndex = 0;
    m_lastApplied = 0;
    m_logs.clear();

    for (int i = 0; i < m_peers.size(); ++i)
    {
        m_matchIndex.push_back(0);
        m_nextIndex.push_back(0);
    }
    m_votedFor = -1;

    m_lastSnapshotIncludeIndex = 0;
    m_lastSnapshotIncludeTerm = 0;
    m_lastResetElectionTime = now();
    m_lastResetHeartBeatTime = now();

    readPersist(m_persister->ReadRaftState()); // 持久化存储中恢复Raft状态
    if (m_lastSnapshotIncludeIndex > 0)
        m_lastApplied = m_lastSnapshotIncludeIndex;

    DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
            m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);

    m_mtx.unlock();

    m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, FIBER_USE_CALLER_THREAD);

    // 产生三个定时器，分别维护：选举，日志同步和心跳， raft节点和kvServer的联系
    m_ioManager->scheduler([this]() -> void
                           { this->leaderHeartBeatTicker(); });
    m_ioManager->scheduler([this]() -> void
                           { this->electionTimeOutTicker(); });

    // applierTicker 时间受数据库响应延迟、两次 apply 之间请求数量的影响，因此用线程更稳。
    std::thread t3(&Raft::applierTicker, this);
    t3.detach();
}

// 循环检测自己是否要发起选举（当没有心跳时），如果该发起选举就执行doElection发起选举
void Raft::electionTimeOutTicker()
{
    while (true)
    {
        // 如果不睡，那么对于Leader，这个函数会一直空转，浪费CPU，且加入协程后，会导致其他协程无法运行
        // 如果自己是 Leader，不发起选举，只是简单 sleep。
        while (m_status == Leader)
        {
            usleep(HeartBeatTimeout);
        }
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleeptime{}; // 初始化一个纳秒级别的时间间隔对象
        std::chrono::system_clock::time_point wakeTime{};                                      // 记录时间节点
        {
            m_mtx.lock();
            wakeTime = now();
            // 基于上一次心跳时间，计算距离下一次超时应该睡眠的时间 如果 suitableSleepTime 很小了，说明快要选举了
            suitableSleeptime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
            m_mtx.unlock();
        }

        if (std::chrono::duration<double, std::milli>(suitableSleeptime).count() > 1) // 如果 suitableSleepTime > 1ms，说明需要 sleep。
        {
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();

            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleeptime).count());

            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();

            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleeptime).count() << " 毫秒\033[0m"
                      << std::endl;
            std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                      << std::endl;
        }

        // 如果睡的这段时间内，有新的心跳（即 m_lastResetElectionTime 更新了，比 wakeTime 大） 说明心跳正常，不需要选举
        if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0)
            continue;

        doElection();
    }
}

// 在选举定时器超时时调用，让当前节点发起一次选举，希望成为新的Leader
void Raft::doElection()
{
    std::lock_guard<std::mutex> g(m_mtx);

    if (m_status != Leader)
    {
        DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);

        // 当选举的时候定时器超时就必须重新选举，不然没有选票就会一直卡住
        // 重竞选超时，term也会增加的
        m_status = Candidate;
        /// 开始新一轮的选举
        m_currentTerm += 1;
        // 即是自己给自己投，也避免candidate给同辈的candidate投
        m_votedFor = m_me;
        persist();

        std::shared_ptr<int> votedNum = std::make_shared<int>(1);

        // 重置选举超时定时器
        m_lastResetElectionTime = now();

        // 向其他所有节点并发发送 RequestVote RPC
        for (int i = 0; i < m_peers.size(); ++i)
        {
            if (i == m_me)
                continue;

            int lastLogIndex = -1, lastLogTerm = -1;
            // 获取当前日志中最后一个 entry 的索引 & term。用于判断候选人日志是否足够新（Raft 的“up-to-date”规则）。
            getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm);

            std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs = std::make_shared<raftRpcProctoc::RequestVoteArgs>();
            requestVoteArgs->set_term(m_currentTerm);
            requestVoteArgs->set_candidateid(m_me);
            requestVoteArgs->set_lastlogindex(lastLogIndex);
            requestVoteArgs->set_lastlogterm(lastLogTerm);

            auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();
            std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply, votedNum); // 创建新线程并执行sendRequestVote函数
            t.detach();
        }
    }
}

// 此时raft节点时candidate状态， 用于向其他raft节点发送RPC请求，以争取成为Leader
bool Raft::sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum)
{
    auto start = now();
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 開始", m_me, m_currentTerm, getLastLogIndex());
    bool ok = m_peers[server]->RequestVote(args.get(), reply.get()); // 接收其他raft节点的返回结果
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 完畢，耗時:{%d} ms", m_me, m_currentTerm,
            getLastLogIndex(), now() - start);

    if (!ok) // RPC通信失败则立即返回
        return ok;

    std::lock_guard<std::mutex> lg(m_mtx);
    // 回应的term比自己的term大，自己落后了，更新状态并退出
    if (reply->term() > m_currentTerm)
    {
        // 当前 candidate 遇到更高 term，要立即退回 Follower
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        persist();
        return true;
    }
    else if (reply->term() < m_currentTerm) // term小，就直接退出
        return true;
    myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));

    if (!reply->votegranted()) // 该节点因某些原因没给本节点投票，结束
        return true;

    *votedNum = *votedNum + 1; // 该节点给自己投票了，投票数加1
    if (*votedNum >= m_peers.size() / 2 + 1)
    {
        // 变成leader
        *votedNum = 0;
        if (m_status == Leader)
        {
            // 如果已经是leader
            myAssert(false, format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
        }
        //	第一次变成leader，初始化状态和nextIndex、matchIndex
        m_status = Leader;

        DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
                getLastLogIndex());

        int lastLogIndex = getLastLogIndex();
        for (int i = 0; i < m_nextIndex.size(); i++)
        {
            m_nextIndex[i] = lastLogIndex + 1;
            m_matchIndex[i] = 0; // 每换一个领导都是从0开始
        }

        std::thread t(&Raft::doHeartBeat, this); // 马上向其他节点宣告自己就是leader， doHeartBeat() 会调用 AppendEntries（心跳）同步所有节点
        t.detach();

        persist();
    }
    return true;
}

// 函数接收来自其他 Raft 节点的 拉票请求，并决定是否投票
void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    DEFER
    {
        persist();
    };

    // 竞选者已经过期 请求 term < 当前 term，直接拒绝
    if (args->term() < m_currentTerm)
    {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Expire);  // Expire 表示“你过期了”。
        reply->set_votegranted(false); // 投票失败
        return;
    }

    if (args->term() > m_currentTerm)
    {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
        // 继续判断是否投票
    }

    myAssert(args->term() == m_currentTerm,
             format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));
    //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了

    int lastLogTerm = getLastLogTerm();
    // 只有请求者的日志“足够新”（Up-to-date）才投票 不能投票给日志比自己“旧”的候选者
    if (!UpToDate(args->lastlogindex(), args->lastlogterm()))
    {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);

        return;
    }

    // 判断是否已经投票过
    if (m_votedFor != -1 && m_votedFor != args->candidateid())
    {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);
        return;
    }
    else
    {
        m_votedFor = args->candidateid();
        m_lastResetElectionTime = now();
        reply->set_term(m_currentTerm);
        reply->set_votestate(Normal);
        reply->set_votegranted(true);
        return;
    }
}

bool Raft::UpToDate(int index, int term)
{
    int lastIndex = -1;
    int lastTerm = -1;
    getLastLogIndexAndTerm(&lastIndex, &lastTerm);
    return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm)
{
    if (m_logs.empty())
    {
        *lastLogIndex = m_lastSnapshotIncludeIndex;
        *lastLogTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    else
    {
        *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
        *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
        return;
    }
}

int Raft::getLastLogIndex()
{
    int lastLogIndex = -1;
    int _ = -1;
    getLastLogIndexAndTerm(&lastLogIndex, &_);
    return lastLogIndex;
}

int Raft::getLastLogTerm()
{
    int _ = -1;
    int lastLogTerm = -1;
    getLastLogIndexAndTerm(&_, &lastLogTerm);
    return lastLogTerm;
}

// 负责查看是否该发送心跳，是则执行doHeartBeat()  Leader 节点用于周期性发送心跳的定时器逻辑
void Raft::leaderHeartBeatTicker()
{
    while (true)
    {
        while (m_status != Leader)
        {
            usleep(1000 * HeartBeatTimeout);
        }
        static std::atomic<int32_t> atomicCount = 0;

        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{}; // 表示当前线程需要睡眠的时间，基于心跳超时时间和上一次心跳重置时间
        std::chrono::system_clock::time_point wakeTime{};
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            wakeTime = now();
            suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHeartBeatTime - wakeTime;
        }

        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1)
        {
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                      << std::endl;
            auto start = std::chrono::steady_clock::now();

            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());

            auto end = std::chrono::steady_clock::now();

            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                      << " 毫秒\033[0m" << std::endl;
            ++atomicCount;
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetHeartBeatTime - wakeTime).count() > 0)
        {
            // 睡眠的这段时间有重置定时器，没有超时，再次睡眠
            continue;
        }
        // DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了\n", m_me);
        doHeartBeat();
    }
}

// 实际发送心跳
void Raft::doHeartBeat()
{
    std::lock_guard<std::mutex> g(m_mtx);

    if (m_status == Leader)
    {
        DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);

        auto appendNums = std::make_shared<int>(1); // 正确返回的节点的数量

        /*
            Leader 会根据每个 Follower 的 nextIndex[i]，决定应该发送哪些日志条目给它。
            每次发送的日志从 nextIndex[i] 开始，到 Leader 当前日志末尾为止。
            如果 nextIndex[i] <= m_lastSnapshotIncludeIndex，说明 Follower 太落后了，Leader 的日志已经被快照截断，必须通过 leaderSendSnapShot 发送快照而不是日志。
            如果不是快照情况，那就根据 prevLogIndex + 1 往后切片发送。
        */
        // 对每个 Follower（除自己外）发送 AppendEntries RPC（包含或不包含日志）
        for (int i = 0; i < m_peers.size(); ++i)
        {
            if (i == m_me)
                continue;

            DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
            myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));

            /*
                如果 follower 的 nextIndex 过旧（小于等于 leader 的快照终止点 m_lastSnapshotIncludeIndex）， 则无法再通过日志同步，只能通过发快照（InstallSnapshot RPC）
            */
            if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex)
            {
                std::thread t(&Raft::leaderSendSnapShot, this, i);
                t.detach();
                continue;
            }

            // 构造发送值
            int preLogIndex = -1; // preLogIndex 是该 follower 预期日志的前一个日志索引
            int preLogTerm = -1;  // PrevLogTerm 是该索引下对应日志的任期
            getPrevLogInfo(i, &preLogIndex, &preLogTerm);
            std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
            appendEntriesArgs->set_term(m_currentTerm);
            appendEntriesArgs->set_leaderid(m_me);
            appendEntriesArgs->set_prevlogindex(preLogIndex);
            appendEntriesArgs->set_prevlogterm(preLogTerm);
            appendEntriesArgs->clear_entries();
            appendEntriesArgs->set_leadercommit(m_commitIndex);

            // 这种情况是 Follower 不是太旧，它的 nextIndex 还在当前日志数组中
            if (preLogIndex != m_lastSnapshotIncludeIndex)
            {
                // 所以只发送部分m_logs
                for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j)
                {
                    raftRpcProctoc::LogEntry *sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = m_logs[j];
                }
            }
            else
            {
                // 这种情况是 Follower 已落后到快照位置，nextIndex = m_lastSnapshotIncludeIndex + 1
                // 发送全部m_logs
                for (const auto &item : m_logs)
                {
                    raftRpcProctoc::LogEntry *sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = item;
                }
            }

            int lastLogIndex = getLastLogIndex();
            // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
            myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
                     format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                            appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));

            // 构造返回值
            const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply = std::make_shared<raftRpcProctoc::AppendEntriesReply>();
            appendEntriesReply->set_appstate(Disconnected);

            std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply, appendNums);
            t.detach();
        }
        m_lastResetHeartBeatTime = now(); // 更新最近一次发送心跳的时间
    }
}

//  Raft 中 Leader 向 Follower 发送 AppendEntries RPC（即心跳或日志同步）的核心实现  还需要负责接收并处理对端发送回来的响应
bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply, std::shared_ptr<int> appendNums)
{
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc開始 ， args->entries_size():{%d}", m_me,
            server, args->entries_size());

    // ok表示网络通信是否成功
    // 通过代理对象 m_peers[server] 向远端节点发起 RPC
    bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());

    if (!ok)
    {
        DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
        return ok;
    }

    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
    if (reply->appstate() == Disconnected)
        return ok;

    std::lock_guard<std::mutex> lg1(m_mtx);

    if (reply->term() > m_currentTerm)
    {
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        return ok;
    }
    else if (reply->term() < m_currentTerm)
    {
        DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
                m_me, m_currentTerm);
        return ok;
    }

    if (m_status != Leader)
    {
        return ok;
    }

    // term相等
    myAssert(reply->term() == m_currentTerm,
             format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));

    // 回退nextIndex 以便下次尝试更前面的日志项
    if (!reply->success()) // 说明此时日志冲突或缺失
    {
        if (reply->updatenextindex() != -100) // updatenextindex()返回了建议回退的索引，用于调整
        {
            DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", m_me,
                    server, reply->updatenextindex());
            m_nextIndex[server] = reply->updatenextindex(); // 更新 nextIndex[server]，用于下次重试时选择更早的日志重新同步
            // 不更新 matchIndex，因为匹配失败了
        }
    }
    else // 日志匹配成功
    {
        *appendNums = *appendNums + 1;
        DPrintf("---------------------------tmp------------------------- 節點{%d}返回true,當前*appendNums{%d}", server,
                *appendNums);

        m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size()); // matchIndex 表示这个 follower 最后一条匹配的日志索引
        m_nextIndex[server] = m_matchIndex[server] + 1;
        int lastLogIndex = getLastLogIndex();

        myAssert(m_nextIndex[server] <= lastLogIndex + 1,
                 format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", server,
                        m_logs.size(), server, lastLogIndex));

        // 超过半数 follower 成功复制后，leader 就可以 commit
        if (*appendNums >= 1 + m_peers.size() / 2)
        {
            *appendNums = 0;

            // Leader 在这次 AppendEntries 中确实发送了日志数据（不是心跳）
            if (args->entries_size() > 0)
            {
                DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                        args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
            }
            // 并非立即就修改 commitIndex，还要满足 只有当前 term 的日志才能提交    领导人只能提交其在当前 term 中的日志条目
            if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm)
            {
                DPrintf(
                    "---------------------------tmp------------------------- 當前term有log成功提交，更新leader的m_commitIndex "
                    "from{%d} to{%d}",
                    m_commitIndex, args->prevlogindex() + args->entries_size());

                m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
            }

            myAssert(m_commitIndex <= lastLogIndex,
                     format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex,
                            m_commitIndex));
        }
    }
    return ok;
}

// Follower 节点接收leader发送的日志请求， 主要用于检查当前日志是否匹配并同步leader的日志到本机
void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply)
{
    std::lock_guard<std::mutex> locker(m_mtx);
    reply->set_appstate(AppNormal);

    //  过期的 Leader
    if (args->term() < m_currentTerm)
    {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(-100);
        DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%d}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
                args->term(), m_me, m_currentTerm);
        return; // 注意从过期的领导人收到消息不要重设定时器
    }

    DEFER { persist(); };

    if (args->term() > m_currentTerm)
    {
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
    }

    myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));

    m_status = Follower; // 这里是有必要的，因为如果candidate收到同一个term的leader的AE，需要变成follower

    m_lastResetElectionTime = now(); // 每次收到合法的 Leader 的 AE 请求，Follower 都应该重置选举超时

    if (args->prevlogindex() > getLastLogIndex())
    {
        // Leader 的日志太“新”，Follower 没跟上，拒绝, leader应该回退nextindex
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(getLastLogIndex() + 1);
        return;
    }
    else if (args->prevlogindex() < m_lastSnapshotIncludeIndex)
    {
        // 该 index 的日志已被快照截断了，Leader 应跳到快照之后的位置
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(m_lastSnapshotIncludeIndex + 1);
    }

    // index和term都匹配，开始日志追加
    if (matchLog(args->prevlogindex(), args->prevlogterm()))
    {
        // 对 entries 中的日志项进行追加（或覆盖）
        for (int i = 0; i < args->entries_size(); i++)
        {
            auto log = args->entries(i);
            if (log.logindex() > getLastLogIndex()) // 新日志，直接添加
            {
                m_logs.push_back(log);
            }
            else
            {
                // 没超过就比较是否匹配，不匹配再更新，而不是直接截断
                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() && m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command())
                {
                    // 相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
                    myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                           " {%d:%d}却不同！！\n",
                                           m_me, log.logindex(), log.logterm(), m_me,
                                           m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                           log.command()));
                }
                if (m_logs[getSlicesIndexFromLogIndex(log.logterm())].logterm() != log.logterm())
                {
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log; // term 不一致，覆盖更新
                }
            }
        }
        myAssert(
            getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
            format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
                   m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));

        if (args->leadercommit() > m_commitIndex)
        {
            m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
        }

        myAssert(getLastLogIndex() >= m_commitIndex,
                 format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                        getLastLogIndex(), m_commitIndex));

        reply->set_success(true);
        reply->set_term(m_currentTerm);

        return;
    }
    else // 日志不匹配 index 相同但 term 不同
    {
        reply->set_updatenextindex(args->prevlogindex());

        for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index)
        {
            if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex()))
            {
                reply->set_updatenextindex(index + 1);
                break;
            }
        }
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        return;
    }
}

void Raft::AppendEntries(google::protobuf::RpcController *controller,
                         const ::raftRpcProctoc::AppendEntriesArgs *request,
                         ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done)
{
    AppendEntries1(request, response);
    done->Run(); // 执行 done->Run() 来 通知 RPC 框架：服务端处理已经完成，结果已经写入 response，可以返回给客户端了
}

std::string Raft::persistData()
{
    BoostPersistRaftNode boostPersistRaftNode;
    boostPersistRaftNode.m_currentTerm = m_currentTerm;
    boostPersistRaftNode.m_votedFor = m_votedFor;
    boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
    boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
    for (auto &item : m_logs)
    {
        boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
    }

    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << boostPersistRaftNode;
    return ss.str();
}

void Raft::persist()
{
    auto data = persistData();
    m_persister->SaveRaftState(data);
}

void Raft::readPersist(std::string data)
{
    if (data.empty())
        return;

    std::stringstream iss(data);
    boost::archive::text_iarchive ia(iss);

    BoostPersistRaftNode boostPersistRaftNode;
    ia >> boostPersistRaftNode;

    m_currentTerm = boostPersistRaftNode.m_currentTerm;
    m_votedFor = boostPersistRaftNode.m_votedFor;
    m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
    m_logs.clear();
    for (auto &item : boostPersistRaftNode.m_logs)
    {
        raftRpcProctoc::LogEntry logEntry;
        logEntry.ParseFromString(item);
        m_logs.emplace_back(logEntry);
    }
}

void Raft::GetState(int *term, bool *isLeader)
{
    m_mtx.lock();
    DEFER
    {
        m_mtx.unlock();
    };

    *term = m_currentTerm;
    *isLeader = (m_status == Leader);
}

// Leader 只有当某条日志被「多数派节点复制」并且属于当前任期（currentTerm）时，才可以将其 commit
// Leader 节点调用，用于 根据各个节点的 matchIndex[]，决定自己当前能 safely 提交到哪里（即更新 commitIndex)
void Raft::leaderUpdateCommitIndex()
{
    m_commitIndex = m_lastSnapshotIncludeIndex; // commitIndex 的起点至少是快照的最后索引

    // 从日志末尾向前找，知道快照尾部
    // 寻找 当前任期内并且被多数节点确认的最大日志条目 index
    for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; --index)
    {
        int sum = 0; // 统计多少节点的 matchIndex >= index
        for (int i = 0; i < m_peers.size(); i++)
        {
            if (i == m_me)
            {
                sum += 1;
                continue;
            }
            if (m_matchIndex[i] >= index)
            {
                sum += 1;
            }
        }

        // 判断这个 index 是否被多数节点「拥有」且 是当前term的日志
        if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm)
        {
            m_commitIndex = index;
            break;
        }
    }
}

bool Raft::matchLog(int logIndex, int logTerm)
{
    // 要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
    myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
             format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                    logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
    return logTerm = getLogTermFromLogIndex(logIndex);
}

// logIndex--log的逻辑（全局）index
int Raft::getLogTermFromLogIndex(int logIndex)
{
    myAssert(logIndex >= m_lastSnapshotIncludeIndex,
             format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));

    int lastLogIndex = getLastLogIndex();

    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                              m_me, logIndex, lastLogIndex));

    if (logIndex == m_lastSnapshotIncludeIndex)
        return m_lastSnapshotIncludeTerm;
    else
        return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
}

int Raft::GetRaftStateSize() { return m_persister->RaftStateSize(); }

// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex)
{
    myAssert(logIndex > m_lastSnapshotIncludeIndex,
             format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));

    int lastLogIndex = getLastLogIndex();
    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                              m_me, logIndex, lastLogIndex));
    int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
    return SliceIndex;
}

/// leader调用，传入：服务器index，传出：发送的AE的preLogIndex和PrevLogTerm
void Raft::getPrevLogInfo(int server, int *preIndex, int *preTerm)
{
    if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1)
    {
        // 要发送的日志是第一个日志，因此直接返回m_lastSnapshotIncludeIndex和m_lastSnapshotIncludeTerm
        *preIndex = m_lastSnapshotIncludeIndex;
        *preTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    auto nextIndex = m_nextIndex[server];
    *preIndex = nextIndex - 1;
    *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

// Leader 处理客户端新命令（command）请求的核心入口 当 Leader 收到一个来自客户端的新命令时，它会将该命令作为新的日志条目添加到日志中，并开始复制给其他服务器
void Raft::Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader)
{
    std::lock_guard<std::mutex> lg1(m_mtx);

    if (m_status != Leader)
    {
        DPrintf("[func-Start-rf{%d}]  is not leader");
        *newLogIndex = -1;
        *newLogTerm = -1;
        *isLeader = false;
        return;
    }

    raftRpcProctoc::LogEntry newLogEntry;
    newLogEntry.set_command(command.asString());
    newLogEntry.set_logterm(m_currentTerm);
    newLogEntry.set_logindex(getNewCommandIndex()); // Leader 把这个命令添加到本地日志尾部（但此时尚未提交，也未应用到状态机）
    m_logs.emplace_back(newLogEntry);

    int lastLogIndex = getLastLogIndex(); // 刚刚添加的新日志的 index

    // leader应该不停的向各个Follower发送AE来维护心跳和保持日志同步，目前的做法是新的命令来了不会直接执行，而是等待leader的心跳触发
    DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);

    persist();
    *newLogIndex = newLogEntry.logindex();
    *newLogTerm = newLogEntry.logterm();
    *isLeader = true;
}

// 获取新命令应该分配的index
int Raft::getNewCommandIndex()
{
    auto lastLogIndex = getLastLogIndex();
    return lastLogIndex + 1;
}

// 用于在日志过长时截断旧日志并保存成一个快照
// 应用层调用此函数，表示状态机已将日志应用到 index，可以生成包含这些日志内容的快照
void Raft::Snapshot(int index, std::string snapshot)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex)
    {
        DPrintf(
            "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
            "smaller ",
            m_me, index, m_lastSnapshotIncludeIndex);
        return;
    }

    auto lastLogIndex = getLastLogIndex();

    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs;

    // 截断日志（只保留 snapshot 之后的）
    for (int i = index + 1; i <= getLastLogIndex(); i++)
    {
        trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }

    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;

    m_logs = trunckedLogs;
    m_commitIndex = std::max(m_commitIndex, index); // ?
    m_lastApplied = std::max(m_lastApplied, index);

    m_persister->Save(persistData(), snapshot);

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
            m_lastSnapshotIncludeTerm, m_logs.size());
    myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
             format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                    m_lastSnapshotIncludeIndex, lastLogIndex));
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot)
{
    return true;
}

// follower 接收并安装 snapshot（快照） 的核心逻辑
void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                           raftRpcProctoc::InstallSnapshotResponse *reply)
{
    m_mtx.lock();
    DEFER
    {
        m_mtx.unlock();
    };

    if (args->term() < m_currentTerm)
    {
        reply->set_term(m_currentTerm);
        return;
    }

    if (args->term() > m_currentTerm)
    {
        m_currentTerm = args->term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
    }
    m_status = Follower;
    m_lastResetElectionTime = now();

    // 若发来的快照比本地已安装的还旧，说明 leader 落后或重发，则直接忽略。
    if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex)
    {
        return;
    }

    auto lastLogIndex = getLastLogIndex();

    // 目标：清除 被snapshot 覆盖的日志
    // 截断日志包括：日志长了，截断一部分，日志短了，全部清空，其实两个是一种情况
    // 但是由于现在getSlicesIndexFromLogIndex的实现，不能传入不存在logIndex，否则会panic
    if (lastLogIndex > args->lastsnapshotincludeindex())
    {
        m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
    }
    else
    {
        m_logs.clear(); // m_logs中的日志已经全部在args传过来的快照中的，所以将m_logs清空
    }

    m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
    m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
    m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
    m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

    reply->set_term(m_currentTerm);
    ApplyMsg msg;
    msg.SnapshotValid = true;
    msg.Snapshot = args->data();
    msg.SnapshotTerm = args->lastsnapshotincludeterm();
    msg.SnapshotIndex = args->lastsnapshotincludeindex();

    std::thread t(&Raft::pushMsgToKvServer, this, msg); // 异步发送 snapshot 应用消息到状态机
    t.detach();

    m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg msg)
{
    applyChan->Push(msg);
}

// /  Leader 向 Follower 发送快照（Snapshot）
void Raft::leaderSendSnapShot(int server)
{
    m_mtx.lock();
    raftRpcProctoc::InstallSnapshotRequest args;
    args.set_leaderid(m_me);
    args.set_term(m_currentTerm);
    args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
    args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
    args.set_data(m_persister->ReadSnapshot());

    raftRpcProctoc::InstallSnapshotResponse reply;
    m_mtx.unlock();
    bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
    m_mtx.lock();
    DEFER
    {
        m_mtx.unlock();
    };
    if (!ok)
    {
        return;
    }
    if (m_status != Leader || m_currentTerm != args.term())
        return; // 中间释放过锁，可能状态已经改变了

    if (reply.term() > m_currentTerm)
    {
        m_currentTerm = reply.term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
        m_lastResetElectionTime = now();
        return;
    }

    m_matchIndex[server] = args.lastsnapshotincludeindex();
    m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::applierTicker()
{
    while (true)
    {
        m_mtx.lock();
        if (m_status == Leader)
        {
            DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
                    m_commitIndex);
        }
        auto applyMsgs = getApplyLogs();
        m_mtx.unlock();

        if (!applyMsgs.empty())
        {
            DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
        }
        for (auto &message : applyMsgs)
        {
            applyChan->Push(message);
        }

        // usleep(1000 * ApplyInterval);
        sleepNMilliseconds(ApplyInterval);
    }
}

// 获取以应用到状态机的日志
// 从未应用的日志中提取 [lastApplied+1, commitIndex] 范围内的日志条目，封装成 ApplyMsg 并返回，供状态机（如 KVServer）执行。
std::vector<ApplyMsg> Raft::getApplyLogs()
{
    std::vector<ApplyMsg> applyMsgs;
    myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                                                        m_me, m_commitIndex, getLastLogIndex()));
    while (m_lastApplied < m_commitIndex)
    {
        m_lastApplied++;
        myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
                 format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                        m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
        ApplyMsg applyMsg;
        applyMsg.commandValid = true;
        applyMsg.SnapshotValid = false;
        applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
        applyMsg.CommandIndex = m_lastApplied;
        applyMsgs.emplace_back(applyMsg);
    }
    return applyMsgs;
}

void Raft::InstallSnapshot(google::protobuf::RpcController *controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest *request,
                           ::raftRpcProctoc::InstallSnapshotResponse *response, ::google::protobuf::Closure *done)
{
    InstallSnapshot(request, response);
    done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                       ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done)
{
    RequestVote(request, response);
    done->Run();
}