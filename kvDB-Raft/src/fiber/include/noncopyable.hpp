#pragma once

namespace monsoon
{
    class Nonecopyable
    {
    public:
        Nonecopyable() = default;
        ~Nonecopyable() = default;
        Nonecopyable(const Nonecopyable &) = delete;
        Nonecopyable operator=(const Nonecopyable &) = delete;
    };
}