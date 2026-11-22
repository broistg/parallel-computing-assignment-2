#pragma once
#include "IListStrategy.h"
#include <memory>
#include <string>

class ConcurrentList {
private:
    std::unique_ptr<IListStrategy> strategy;

public:
    explicit ConcurrentList(std::unique_ptr<IListStrategy> strat)
        : strategy(std::move(strat)) {}

    bool add(int id, const std::string& name) {
        return strategy->add(id, name);
    }

    bool remove(int id) {
        return strategy->remove(id);
    }

    bool contains(int id) {
        return strategy->contains(id);
    }

    void print() {
        return strategy->print();
    }
};