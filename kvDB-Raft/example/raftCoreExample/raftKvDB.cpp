//
// Created by swx on 23-12-28.
//
#include <iostream>
#include "raft.h"
// #include "kvServer.h"
#include <kvServer.h>
#include <unistd.h>
#include <iostream>
#include <random>


/*
    这段代码负责 批量启动多个 KVServer 节点进程，并将这些节点组成一个分布式 Raft 集群，它会：

    解析命令行参数（节点数和配置文件名）。
    为每个节点生成端口号（随机起始 + 连续分配）。
    通过 fork() 创建多个进程，每个进程启动一个 KvServer 实例。
    每个 KvServer 的构造函数中，会初始化 Raft 节点 m_raftNode。
*/

void ShowArgsHelp();

int main(int argc, char **argv)
{
    //////////////////////////////////读取命令参数：节点数量、写入raft节点节点信息到哪个文件
    if (argc < 2)
    {
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }
    int c = 0;
    int nodeNum = 0;
    std::string configFileName;

    // 使用随机数生成器，随机分配一个起始端口号（10000~29999之间）
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 29999);
    unsigned short startPort = dis(gen);


    while ((c = getopt(argc, argv, "n:f:")) != -1) // 解析命令行参数
    {
        switch (c)
        {
        case 'n':
            nodeNum = atoi(optarg);
            break;
        case 'f':
            configFileName = optarg;
            break;
        default:
            ShowArgsHelp();
            exit(EXIT_FAILURE);
        }
    }
    std::ofstream file(configFileName, std::ios::out | std::ios::app); 
    file.close();
    file = std::ofstream(configFileName, std::ios::out | std::ios::trunc); // 清空旧的配置文件，为后续写入节点信息做准备
    if (file.is_open())
    {
        file.close();
        std::cout << configFileName << " 已清空" << std::endl;
    }
    else
    {
        std::cout << "无法打开 " << configFileName << std::endl;
        exit(EXIT_FAILURE);
    }

    // 启动多个子进程，每个进程负责启动一个 KVServer 节点
    for (int i = 0; i < nodeNum; i++)
    {
        short port = startPort + static_cast<short>(i);
        std::cout << "start to create raftkv node:" << i << "    port:" << port << " pid:" << getpid() << std::endl;
        pid_t pid = fork(); // 创建新进程
        if (pid == 0)
        {
            // 如果是子进程
            // 子进程的代码

            auto kvServer = new KvServer(i, 500, configFileName, port); 
            pause(); // 子进程进入等待状态，不会执行 return 语句
        }
        else if (pid > 0)
        {
            // 如果是父进程
            // 父进程的代码
            sleep(1);
        }
        else
        {
            // 如果创建进程失败
            std::cerr << "Failed to create child process." << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    pause();
    return 0;
}

void ShowArgsHelp() { std::cout << "format: command -n <nodeNum> -f <configFileName>" << std::endl; }