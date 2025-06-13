# Clerk客户端

## `clerk.h` 

`Clerk` 是**客户端角色**，代表一个用于访问分布式 KV 服务（Key-Value Server）的客户端模块。它封装了 `PutAppend` 与 `Get` 操作，通过 RPC 调用与远端 Raft KVServer 进行通信。

**每个KVServer在同一主机的不同进程或不同主机中，Clerk需要通过网络将请求发送给KVserver。KVserver收到 Clerk 的请求后，不会直接应用到数据库中。它会先将操作封装成一个请求发送给该server对应的Raft节点，Raft集群中的Leader将此请求分发给其余raft节点，只有当该命令被 多数Raft节点 应用（即成为已提交的日志并应用到状态机）后，KVServer 才真正修改本地的 KV 数据。**

![](.\imgs\Raft模块图.png)

##  `raftServerRpcUtil.h` 

封装了raftKVRpcProctoc::kvServerRpc_Stub， Clerk通过这个对象与KVServer节点进行通信（发送Get，Put请求）。

