# `RPC`

`mprpcchannel.h`此类继承了`protobuf`自动生成的`RpcChannel`类，并重写`CallMethod`虚函数，使得通过`stub`进行`RPC`调用时最终都会调用到这个重写的`CallMethod`函数。`stub`是 `Google Protobuf` 的 `RPC` 框架自动生成的**客户端代理类**的对象。作用是**在客户端本地充当远程服务的“代理”，将客户端的调用转化为远程服务调用的网络请求**。这个类会根据编写好的`.proto`文件，自动生成的调用方法，但在这些方法并不实现业务逻辑，而是转调用 `channel`的`callMethod`方法。

​	在重写的`CallMethod`方法中会将本次请求组织序列化，并发送给`RPC`服务端，等待响应后将其解析填充到`response`中返回。

`rpcprovider.h`负责发布本地服务（`NotifyService`），启动网络监听开始提供`RPC`远程网络调用服务（`Run`），接收客户端请求，解析协议，调用对应服务方法（`OnMessage`），返回结果给客户端（`SendRpcResponse`）

`mprpccontroller.h`用于在一次`RPC`调用期间传递和保存调用状态



本项目使用的`RPC`高度依赖于`protobuf`

`RPC`是一种使得分布式系统中的不同模块之间能够透明地进行远程调用的技术，使得开发者可以更方便地构建分布式系统，而不用过多关注底层通信细节，调用另一台机器的方法会表现得像调用本地方法一样。

## `RPC`通信流程

![](C:\HHN\c++\project\kvStorageBasedRaft\notes\imgs\RPC通信流程图.png)

### `RPC`请求构造和发送：客户端侧（`RpcChannel`）

客户端通过 `stub->PutAppend()` 的方式发送 `RPC`，这背后由 `MprpcChannel::CallMethod` 实现

### 服务端 `RPC` 处理器：`RpcProvider`

`Run`中会启动`muduo_server`开启监听,当有远程`Rpc`请求发送过来时，会触发`OnMessage`回调函数，对请求解析，并调用函数获取结果，最后将结果序列化后发送回调用端。



`notifyService`方法，需要传入一个 继承了`protobuf`自动生成的`serviceRpc`类 的`service`类，才能将将服务注册好。

