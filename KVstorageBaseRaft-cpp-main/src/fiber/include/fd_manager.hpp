#ifndef __FD_MANAGER_H__
#define __FD_MANAGER_H__

/**
 * 实现了一个文件描述符（FD）管理器，用于管理和封装每一个文件描述符（尤其是 socket) 的各种状态与属性（是否非阻塞、是否关闭、超时时间等）
 */

#include <memory>
#include <vector>
#include "mutex.hpp"
#include "singleton.hpp"
#include "thread.hpp"

namespace monsoon
{
  // 管理单个文件句柄类型，阻塞，关闭，读写超时
  // 继承 enable_shared_from_this 表明该对象会被用 std::shared_ptr 管理。
  class FdCtx : public std::enable_shared_from_this<FdCtx>
  {
  public:
    typedef std::shared_ptr<FdCtx> ptr;

    FdCtx(int fd);
    ~FdCtx();
    // 是否完成初始化
    bool isInit() const { return m_isInit; }
    // 是否是socket
    bool isSocket() const { return m_isSocket; }
    // 是否已经关闭
    bool isClose() const { return m_isClosed; }
    // 用户主动设置非阻塞
    void setUserNonblock(bool v) { m_userNonblock = v; }
    // 用户是否主动设置了非阻塞
    bool getUserNonblock() const { return m_userNonblock; }
    // 设置系统非阻塞
    void setSysNonblock(bool v) { m_sysNonblock = v; }
    // 获取系统是否非阻塞
    bool getSysNonblock() const { return m_sysNonblock; }
    // 设置超时时间
    void setTimeout(int type, uint64_t v);
    uint64_t getTimeout(int type);

  private:
    bool init();

  private:
    /// 是否初始化
    bool m_isInit : 1;
    /// 是否socket
    bool m_isSocket : 1;
    /// 是否hook非阻塞
    bool m_sysNonblock : 1;
    /// 是否用户主动设置非阻塞
    bool m_userNonblock : 1;
    /// 是否关闭
    bool m_isClosed : 1;
    /// 文件句柄
    int m_fd;
    /// 读超时时间毫秒
    uint64_t m_recvTimeout;
    /// 写超时时间毫秒
    uint64_t m_sendTimeout;
  };


  // 管理多个FdCtx
  class FdManager
  {
  public:
    typedef RWMutex RWMutexType;

    FdManager();
    // 获取/创建文件句柄类
    // auto_create 是否自动创建
    FdCtx::ptr get(int fd, bool auto_create = false);
    // 删除文件句柄
    void del(int fd);

  private:
    /// 读写锁
    RWMutexType m_mutex;
    /// 文件句柄集合
    std::vector<FdCtx::ptr> m_datas;
  };

  /// 文件句柄单例
  typedef Singleton<FdManager> FdMgr;

} // namespace monsoon

#endif