#pragma once

#include <fstream>
#include <mutex>
/**
 * Persister 是 Raft 中负责持久化日志、持久化快照的模块。
 * 保障了：即使节点宕机、重启，数据也能正确恢复，从而维持 Raft 的一致性和正确性。
 */
class Persister
{
private:
    std::mutex m_mtx;
    std::string m_raftState;
    std::string m_snapshot;

    // Raft状态保存到磁盘时的文件名
    const std::string m_raftStateFileName;
    // 快照保存到磁盘时的文件名
    const std::string m_snapshotFileName;
    // 持久化用的输出流对象。
    std::ofstream m_raftStateOutStream;
    std::ofstream m_snapshotOutStream;
    /**
     * raftState在内存中的大小。
     * 避免每次都读取文件来获取具体的大小
     */
    long long m_raftStateSize;

public:
    /**
     * 一次性同时保存新的 raftState 和 snapshot
     * 适合在 Snapshot 事件发生时调用（因为 snapshot 保存后，raftState也同步变了）
     */
    void Save(std::string raftstate, std::string snapshot);
    std::string ReadSnapshot();
    // 只保存 raftState，不动 snapshot。
    void SaveRaftState(const std::string &data);
    long long RaftStateSize();
    std::string ReadRaftState();
    explicit Persister(int me);
    ~Persister();

private:
    void clearRaftState();
    void clearSnapshot();
    void clearRaftStateAndSnapshot();
};