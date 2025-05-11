# Raft

![](C:\HHN\c++\project\kvStorageBasedRaft\notes\imgs\Raft模块图.png)

## `ApplyMsg.h`

是 Raft 向上层（KV Server）提交指令（Command）或者快照（Snapshot）时，用的消息载体。

## `raftRpcUtil.h`

封装了raftRpcProctoc::raftRpc_Stub，用这个对象可以让raft节点与其他raft节点进行RPC通信（AppendEntries、RequestVote等）。

## `Persister.h`

Raft 中负责持久化日志、持久化快照的模块。

## `raft.h`

`std::shared_ptr<LockQueue<ApplyMsg>> applyChan;` Raft 模块和 KVServer 模块之间通信的桥梁

`KVServer`构造函数中创建`applyChan`，并在`m_raftNode->init(servers, m_me, persister, applyChan);`初始化`raft`节点时，将这个`applyChan`传递给`raft`节点，之后`raft`节点就通过这个对象给`kvServer`传递已经被 Raft 提交并应用的日志命令（`ApplyMsg`）。

#### 主要实现的功能有

1. 选举时间超时，节点从`Follower`变为`Candidate`，并主动发起选举，向其他`Raft`节点发送`RequestVote`请求，如果收到大多数票，则成为`Leader`，如果收到`term`更大的`reply`，则退回到`Follower`。

   ![](C:\HHN\c++\project\kvStorageBasedRaft\notes\imgs\leader选举流程.png)

2. 日志复制（心跳）`sendAppendEntries`， 只有`Leader`才要向其他`raft`节点发送日志或者心跳，并且当多数`raft`节点成功复制了日志，本条日志可以提交。Leader会在后台每隔固定时间循环发送心跳。

   ![](C:\HHN\c++\project\kvStorageBasedRaft\notes\imgs\日志复制与心跳流程.png)

3. 日志操作工具，包括判断传来的日志是否与本节点匹配、、、

4. 状态持久化

5. 将多数节点已成功复制的日志命令 回送 给`KVServer` 应用到数据库

6. 与快照相关的功能

7. Leader处理KvServer发送过来的客户端请求的接口。`Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader);`Leader会将该命令作为新的日志条目添加到日志中，并开始复制给其他Raft节点。

### 整体运作流程：

1. Clerk 发出 `Put` 请求到某个 KVServer
2.  KVServer通过Raft的Start接口将命令传给Raft节点（可能是 Leader，也可能不是）
3. 若不是 Leader，返回 `ErrWrongLeader`。
4. Leader 将该命令追加到本地日志，并返回该日志`index`
5. Leader 在doHeartBeat创建多个线程并发向其他raft发送 `sendAppendEntries()` 复制日志, 若大多数节点确认，更新 `commitIndex`
6. 后台线程 `applierTicker()` 检测到 commitIndex 更新， 从日志中读取命令，并通过 `applyChan` 通知 KVServer
7. KVServer 读取 `applyChan` 中的 `ApplyMsg`，执行命令，响应 Clerk。

### 关键问题

1. 如果同时有多个Follower同时选举超时，最终的结果就是无人当选Leader，然后重置随机的选举超时时间，等待下一次选举

## `kvServer.h`

`kvServer`就是一个中间节点，负责沟通`kvDB`和`raft`节点

每个 KvServer 节点负责初始化自身的 `Raft节点`，将所有`Raft`节点连接起来。所以在实际使用场景中，服务端只需要创建若干个`Server`节点即可。客户端则使用`clerk`来使用服务端。KvServer 构造还会创建自身的raft节点、raft节点共用的applyChan，以及与其他raft节点的连接，并传递给raft节点。

### kvServer功能

1. 接收并处理客户端请求，处理 `Clerk` 发来的 `PutAppend`、`Get` 请求，重写了Rpc框架内的这两个方法，当`clerk`调用`m_servers[server]->PutAppend(&args, &reply)`实际通过`mprpcchannel`中的`CallMethod`调用到了重写的这两个函数。在这两个函数内会将`clerk`发过来的请求包装成`Op`，使用`raft`的`Start`方法让`Leader`节点开始分发该日志，等待日志被应用（大多数`raft`节点复制）后，在将结果响应给`clerk`。
2. 与Raft节点沟通

### 关键执行流程（以 Put 为例）

1. Clerk 发起请求 → KVServer RPC 处理

   - 反序列化请求为 `Op` 结构体

   - 使用 `Start(op)` 通知raft Leader节点发起日志复制，获取 `logIndex`
   - 在 `waitApplyCh[logIndex]` 注册一个通道，阻塞等待结果
   - 如果不是 Leader，立即返回错误

2. Raft 日志复制与提交

   - Raft 将 Op 写入本地日志，发送 AppendEntries
   - 超过半数节点响应后，更新 `commitIndex`
   - Raft 后台线程将 committed 日志通过 `applyChan->push(ApplyMsg)` 发送给 KVServer

3.  KVServer 应用日志 → 状态更新

   - 后台线程从 `applyChan->pop()` 获取 ApplyMsg
   - 根据 Op 的 clientId + seqId 判断是否已经处理（幂等性判断）
   - 若未处理，则执行 skipList 的 Put 或 Append
   - 更新 `clientLastSeq` 与 `clientLastResult`
   - 根据 logIndex 找到对应 waitChan，推送结果唤醒等待的 Clerk

4. Clerk 收到响应，完成请求

