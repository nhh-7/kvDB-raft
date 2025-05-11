#pragma once
#include <google/protobuf/service.h>
#include <string>

// 用于在一次 RPC 调用期间传递和保存调用状态。
class MprpcController : public google::protobuf::RpcController
{
public:
    MprpcController();
    void Reset();
    bool Failed() const;
    std::string ErrorText() const;
    void SetFailed(const std::string &reason);

    void StartCancel();
    bool IsCanceled() const;
    void NotifyOnCancel(google::protobuf::Closure *callback);

private:
    bool m_failed;         // RPC方法执行过程中的状态
    std::string m_errText; // RPC方法执行过程中的错误信息
};