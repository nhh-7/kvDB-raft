#pragma once

#include <memory>

namespace monsoon
{
    // 其实没用上
    // 匿名命名空间：防止外部访问这些函数（仅限于当前文件）
    namespace
    {
        template <class T, class X, int N>
        T &GetInstanceX()
        {
            static T v;
            return v;
        }

        template <class T, class X, int N>
        std::shared_ptr<T> GetInstancePtr()
        {
            static std::shared_ptr<T> v(new T);
            return v;
        }
    }

    // 同一个类型 T，可以通过传入不同的 X（Tag 类型）和 N（索引）来创建多个互不干扰的单例对象！
    /**
     * @brief 单例模式封装类
     * @details T 类型
     *          X 为了创造多个实例对应的Tag
     *          N 同一个Tag创造多个实例索引
     */
    template <class T, class X = void, int N = 0>
    class Singleton
    {
    public:
        /**
         * @brief 返回单例裸指针
         */
        static T *GetInstance()
        {
            static T v;
            return &v;
        }
    };

    /**
     * @brief 单例模式智能指针封装类
     * @details T 类型
     *          X 为了创造多个实例对应的Tag
     *          N 同一个Tag创造多个实例索引
     */
    template <class T, class X = void, int N = 0>
    class SingletonPtr
    {
    public:
        // 返回单例智能指针
        static std::shared_ptr<T> GetInstance()
        {
            static std::shared_ptr<T> v(new T);
            return v;
        }
    };
}