#pragma once
#include <string>

class IListStrategy {
public:
    virtual ~IListStrategy() = default;
    virtual bool add(int id, const std::string& name) = 0;
    virtual bool remove(int id) = 0;
    virtual bool contains(int id) = 0;
    virtual void print() = 0;
};