#pragma once
///@file

#include "nix/util/fun.hh"
#include "nix/util/task.hh"

#include <cassert>
#include <cstddef>
#include <limits>
#include <queue>
#include <set>

namespace nix {

/**
 * The edges from one element of a graph, as a task.
 *
 * The element stays valid until the task completes or is destroyed, so
 * the task can keep a reference to it.
 */
template<typename T>
using GetEdgesTask = fun<Task<std::set<T>>(const T & elt)>;

/**
 * No limit on the number of edge tasks that `computeClosure` runs at the
 * same time.
 */
constexpr size_t unboundedConcurrency = std::numeric_limits<size_t>::max();

/**
 * Compute the closure of `startElts` under `getEdges`, as a task.
 *
 * Calls `getEdges` once for each element of the closure that is not in
 * `known`, and runs up to `maxConcurrent` of the resulting tasks at the
 * same time. Returns `known` plus the closure. An element of `known` is
 * not traversed: a caller can pass the result of an earlier traversal to
 * avoid repeating it.
 *
 * If an edge task throws, `computeClosure` cancels the other edge tasks
 * and rethrows. Destroying the task cancels the edge tasks that run.
 */
template<typename T>
Task<std::set<T>>
computeClosure(std::set<T> startElts, GetEdgesTask<T> getEdges, std::set<T> known = {}, size_t maxConcurrent = 1024)
{
    assert(maxConcurrent > 0);

    /* `res` is declared before `running`, so it outlives the edge tasks,
       which refer to its elements. The nodes of a `std::set` do not move. */
    std::set<T> res = std::move(known);
    std::queue<const T *> todo;
    TaskSet<std::set<T>> running;

    auto enqueue = [&](std::set<T> elts) {
        for (auto & elt : elts)
            if (auto [i, inserted] = res.insert(elt); inserted)
                todo.push(&*i);
    };

    enqueue(std::move(startElts));

    while (true) {
        while (!todo.empty() && running.size() < maxConcurrent) {
            running.push(getEdges(*todo.front()));
            todo.pop();
        }
        auto edges = co_await running.next();
        if (!edges)
            break;
        enqueue(std::move(*edges));
    }

    /* `running` is empty, so nothing refers to `res` any more. */
    co_return std::move(res);
}

} // namespace nix
