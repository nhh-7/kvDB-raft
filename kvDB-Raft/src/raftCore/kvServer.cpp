#include "kvServer.h"
#include "rpcprovider.h"
#include "mprpcconfig.h"

void KvServer::DprintfKVDB()
{
    if (!Debug)
        return;

    std::lock_guard<std::mutex> lg(m_mtx);
    DEFER
    {
        m_skipList.display_list();
    };
}

void KvServer::ExecuteAppendOpOnKVDB(Op op)
{
    m_mtx.lock();

    m_skipList.insert_set_element(op.Key, op.Value);

    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    DprintfKVDB();
}

void KvServer::ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist)
{
    m_mtx.lock();
    *value = "";
    *exist = false;

    if (m_skipList.search_element(op.Key, *value))
        *exist = true;

    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    DprintfKVDB();
}

void KvServer::ExecutePutOpOnKVDB(Op op)
{
    m_mtx.lock();
    m_skipList.insert_set_element(op.Key, op.Value);
    m_lastRequestId[op.ClientId] = op.RequestId;
    m_mtx.unlock();

    DprintfKVDB();
}

// 处理来自clerk的Get RPC
void KvServer::Get(const raftKVRpcProctoc::GetArgs *args, raftKVRpcProctoc::GetReply *reply)
{
    // 将 Get 请求包装为 Op 对象，提交给 Raft
    Op op;
    op.Operation = "Get";
    op.Key = args->key();
    op.Value = "";
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();

    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;
    m_raftNode->Start(op, &raftIndex, &_, &isLeader);

    if (!isLeader)
    {
        reply->set_err(ErrWrongLeader);
        return;
    }

    m_mtx.lock();

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
    {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>()));
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];

    m_mtx.unlock();

    Op raftCommitOp;
    // 超时，没获取到对应的命令
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp))
    {
        int _ = -1;
        bool isLeader = false;
        m_raftNode->GetState(&_, &isLeader);

        // 如果是 Leader，而且该请求之前已经处理过
        if (ifRequestDuplicate(op.ClientId, op.RequestId) && isLeader)
        {
            std::string value;
            bool exist = false;
            // 即使没等到 Raft applyCh 消息，我们也可以从 跳表中 拿出结果
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist)
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else
        {
            reply->set_err(ErrWrongLeader);
        }
    }
    else // 正常获取到 raftCommitOp
    {
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId)
        {
            std::string value;
            bool exist = false;
            ExecuteGetOpOnKVDB(op, &value, &exist);
            if (exist)
            {
                reply->set_err(OK);
                reply->set_value(value);
            }
            else
            {
                reply->set_err(ErrNoKey);
                reply->set_value("");
            }
        }
        else
        {
            reply->set_err(ErrWrongLeader);
        }
    }

    m_mtx.lock();
    // 清理等待通道资源
    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;
    m_mtx.unlock();
}

void KvServer::GetCommandFromRaft(ApplyMsg message)
{
    Op op;
    op.parseFromString(message.Command); // 将 Raft 日志内容解析为 Op 对象。

    DPrintf(
        "[KvServer::GetCommandFromRaft-kvserver{%d}] , Got Command --> Index:{%d} , ClientId {%s}, RequestId {%d}, "
        "Opreation {%s}, Key :{%s}, Value :{%s}",
        m_me, message.CommandIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    // 若日志索引已经包含在快照中，跳过
    if (message.CommandIndex <= m_lastSnapshotRaftLogIndex)
        return;

    // 如果是新请求，则执行到状态机
    if (!ifRequestDuplicate(op.ClientId, op.RequestId))
    {
        if (op.Operation == "Put")
        {
            ExecutePutOpOnKVDB(op);
        }
        if (op.Operation == "Append")
        {
            ExecuteAppendOpOnKVDB(op);
        }
    }

    if (m_maxRaftState != -1)
    {
        IfNeedToSendSnapShotCommand(message.CommandIndex, 9);
    }

    SendMessageToWaitChan(op, message.CommandIndex);
}

// 判断某个客户端请求是否是重复请求 防止 Raft apply 时重复执行。
bool KvServer::ifRequestDuplicate(std::string ClientId, int RequestId)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    if (m_lastRequestId.find(ClientId) == m_lastRequestId.end())
        return false;

    return RequestId <= m_lastRequestId[ClientId];
}

// 处理来自 Clerk 的 Put 或 Append 请求
void KvServer::PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply)
{
    Op op;
    op.Operation = args->op();
    op.Key = args->key();
    op.Value = args->value();
    op.ClientId = args->clientid();
    op.RequestId = args->requestid();
    int raftIndex = -1;
    int _ = -1;
    bool isLeader = false;

    // 将客户端的 Put/Append 请求封装为 Op，调用 Raft 的 Start() 接口发起日志复制
    m_raftNode->Start(op, &raftIndex, &_, &isLeader); // 拿到 raftIndex 后，用于标识该日志条目的索引号

    if (!isLeader)
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , but "
            "not leader",
            m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);
        reply->set_err(ErrWrongLeader);
        return;
    }
    DPrintf(
        "[func -KvServer::PutAppend -kvserver{%d}]From Client %s (Request %d) To Server %d, key %s, raftIndex %d , is "
        "leader ",
        m_me, &args->clientid(), args->requestid(), m_me, &op.Key, raftIndex);

    m_mtx.lock();
    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
    {
        waitApplyCh.insert(std::make_pair(raftIndex, new LockQueue<Op>())); // 为当前 raftIndex 创建一个等待通道，后续 ReadRaftApplyCommandLoop() 中的消息会通过这个通道唤醒 RPC 线程。
    }
    auto chForRaftIndex = waitApplyCh[raftIndex];
    m_mtx.unlock();

    Op raftCommitOp;

    // RPC 线程阻塞等待 Raft apply 后发回的 Op
    if (!chForRaftIndex->timeOutPop(CONSENSUS_TIMEOUT, &raftCommitOp))
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]TIMEOUT PUTAPPEND !!!! Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %s, Opreation %s Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        if (ifRequestDuplicate(op.ClientId, op.RequestId))
        {
            reply->set_err(OK);
        }
        else
        {
            reply->set_err(ErrWrongLeader);
        }
    }
    else // 取到数据
    {
        DPrintf(
            "[func -KvServer::PutAppend -kvserver{%d}]WaitChanGetRaftApplyMessage<--Server %d , get Command <-- Index:%d , "
            "ClientId %s, RequestId %d, Opreation %s, Key :%s, Value :%s",
            m_me, m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
        if (raftCommitOp.ClientId == op.ClientId && raftCommitOp.RequestId == op.RequestId)
        {
            reply->set_err(OK);
        }
        else
        {
            reply->set_err(ErrWrongLeader);
        }
    }

    m_mtx.lock();

    auto tmp = waitApplyCh[raftIndex];
    waitApplyCh.erase(raftIndex);
    delete tmp;

    m_mtx.unlock();
}

// 不断从 applyChan 中读取来自 Raft 的 ApplyMsg 消息
void KvServer::ReadRaftApplyCommandLoop()
{
    while (true)
    {
        auto message = applyChan->Pop(); // 不断从 applyChan 中读取 Raft 层传来的 ApplyMsg
        DPrintf(
            "---------------tmp-------------[func-KvServer::ReadRaftApplyCommandLoop()-kvserver{%d}] 收到了下raft的消息",
            m_me);

        if (message.commandValid)
        {
            GetCommandFromRaft(message);
        }
        if (message.SnapshotValid)
        {
            GetSnapShotFromRaft(message);
        }
    }
}

//  关于快照raft层与persist的交互：保存kvserver传来的snapshot；生成leaderInstallSnapshot RPC的时候也需要读取snapshot；
//  因此snapshot的具体格式是由kvserver层来定的，raft只负责传递这个东西
//  snapShot里面包含kvserver需要维护的persist_lastRequestId 以及kvDB真正保存的数据persist_kvdb
void KvServer::ReadSnapShotToInstall(std::string snaphshot)
{
    if (snaphshot.empty())
        return;

    parseFromString(snaphshot);
}

// Raft状态机在日志成功应用之后 用于通知前端 RPC 线程（如 Get/PutAppend）“你的请求已经提交并应用完成”的桥梁函数
bool KvServer::SendMessageToWaitChan(const Op &op, int raftIndex)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);

    if (waitApplyCh.find(raftIndex) == waitApplyCh.end())
        return false;
    waitApplyCh[raftIndex]->Push(op);
    DPrintf(
        "[RaftApplyMessageSendToWaitChan--> raftserver{%d}] , Send Command --> Index:{%d} , ClientId {%d}, RequestId "
        "{%d}, Opreation {%v}, Key :{%v}, Value :{%v}",
        m_me, raftIndex, &op.ClientId, op.RequestId, &op.Operation, &op.Key, &op.Value);
    return true;
}

void KvServer::IfNeedToSendSnapShotCommand(int raftIndex, int proportion)
{
    if (m_raftNode->GetRaftStateSize() > m_maxRaftState / 10.0)
    {
        auto snapshot = MakeSnapShot();
        m_raftNode->Snapshot(raftIndex, snapshot);
    }
}

void KvServer::GetSnapShotFromRaft(ApplyMsg message)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_raftNode->CondInstallSnapshot(message.SnapshotTerm, message.SnapshotIndex, message.Snapshot))
    {
        ReadSnapShotToInstall(message.Snapshot);
        m_lastSnapshotRaftLogIndex = message.SnapshotIndex;
    }
}

std::string KvServer::MakeSnapShot()
{
    std::lock_guard<std::mutex> lg(m_mtx);
    std::string snapshotData = getSnapshotData();
    return snapshotData;
}

void KvServer::PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                         ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done)
{
    KvServer::PutAppend(request, response);
    done->Run();
}

void KvServer::Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
                   ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done)
{
    KvServer::Get(request, response);
    done->Run();
}

// @param me 当前节点编号 一个节点包含raft和kvserver
// @param nodeInfoFileName: 含有所有节点 IP+Port 的配置文件名
// @param port: 当前服务监听的 RPC 端口
// 每个 KvServer 节点负责初始化自身的 RaftNode，连接其他节点等
KvServer::KvServer(int me, int maxraftstate, std::string nodeInfoFileName, short port) : m_skipList(6)
{
    std::shared_ptr<Persister> persister = std::make_shared<Persister>(me);

    m_me = me;
    m_maxRaftState = maxraftstate;

    applyChan = std::make_shared<LockQueue<ApplyMsg>>(); // Raft 写入日志后将指令发送到这里，供 KV 应用。

    m_raftNode = std::make_shared<Raft>();

    std::thread t([this, port]() -> void
                  {
        RpcProvider provider;
        provider.NotifyService(this);
        provider.NotifyService(this->m_raftNode.get());
        // 启动一个rpc服务发布节点   Run以后，进程进入阻塞状态，等待远程的rpc调用请求
        provider.Run(m_me, port); });
    t.detach();

    ////开启rpc远程调用能力，需要注意必须要保证所有节点都开启rpc接受功能之后才能开启rpc远程调用能力
    ////这里使用睡眠来保证
    std::cout << "raftServer node:" << m_me << " start to sleep to wait all ohter raftnode start!!!!" << std::endl;
    sleep(6);
    std::cout << "raftServer node:" << m_me << " wake up!!!! start to connect other raftnode" << std::endl;

    // 获取所有raft节点ip、port ，并进行连接  ,要排除自己
    MprpcConfig config;
    config.LoadConfigFile(nodeInfoFileName.c_str());
    std::vector<std::pair<std::string, short>> ipPortVt;
    for (int i = 0; i < INT_MAX - 1; ++i)
    {
        std::string node = "node" + std::to_string(i);
        std::string nodeIp = config.Load(node + "ip");
        std::string nodePortStr = config.Load(node + "port");
        if (nodeIp.empty())
            break;

        ipPortVt.emplace_back(nodeIp, atoi(nodePortStr.c_str()));
    }
    std::vector<std::shared_ptr<RaftRpcUtil>> servers;

    // 进行连接
    for (int i = 0; i < ipPortVt.size(); ++i)
    {
        if (i == m_me)
        {
            servers.push_back(nullptr);
            continue;
        }
        std::string otherNodeIp = ipPortVt[i].first;
        short otherNodePort = ipPortVt[i].second;
        auto *rpc = new RaftRpcUtil(otherNodeIp, otherNodePort);
        servers.push_back(std::shared_ptr<RaftRpcUtil>(rpc));

        std::cout << "node" << m_me << " 连接node" << i << "success!" << std::endl;
    }

    sleep(ipPortVt.size() - me); // 等待所有节点相互连接成功，再启动raft
    m_raftNode->init(servers, m_me, persister, applyChan);
    // kv的server直接与raft通信，但kv不直接与raft通信，所以需要把ApplyMsg的chan传递下去用于通信，两者的persist也是共用的

    m_skipList;
    waitApplyCh;
    m_lastRequestId;
    m_lastSnapshotRaftLogIndex = 0;
    auto snapshot = persister->ReadSnapshot();
    if (!snapshot.empty())
        ReadSnapShotToInstall(snapshot);

    std::thread t2(&KvServer::ReadRaftApplyCommandLoop, this);
    t2.join(); //由於ReadRaftApplyCommandLoop一直不會結束，达到一直卡在这的目的
}