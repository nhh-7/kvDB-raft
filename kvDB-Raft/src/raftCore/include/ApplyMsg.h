#pragma once

#include <string>

/**
 * ApplyMsg 是 Raft 向上层（比如状态机、KV Server）提交指令（Command）或者快照（Snapshot）时，用的消息载体。
 */
class ApplyMsg
{
public:
    bool commandValid;    // 表示是否有一个日志条目需要应用到状态机。
    std::string Command;  // 实际的指令（内容），比如 "put key value" 这种。
    int CommandIndex;     // 指令在 Raft 日志里的 index（从1开始编号）。
    bool SnapshotValid;   // 表示是否要安装快照。
    std::string Snapshot; // 快照的内容。
    int SnapshotTerm;     // 快照时的 term，代表历史信息。
    int SnapshotIndex;    // 快照时最后一个日志条目的 index。
public:
    ApplyMsg()
        : commandValid(false), Command(), CommandIndex(-1), SnapshotValid(false), SnapshotTerm(-1), SnapshotIndex(-1)
    {
    }
};