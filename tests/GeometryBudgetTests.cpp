#include "Processor/GeometryBudget.h"
#include <array>
#include <atomic>
#include <barrier>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

namespace
{
using zenasset::GeometryBudget;
using zenasset::GeometryReservation;

bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

bool Arithmetic()
{
    GeometryBudget budget(10);
    bool           valid = Check(budget.Reserve(2, 4) && budget.Used() == 8 && budget.Peak() == 8,
                                 "Count/stride reservation must charge exact requested bytes");
    valid &= Check(!budget.Reserve(3) && budget.Used() == 8, "Failed quota reservation must leave accounting unchanged");
    valid &= Check(!budget.Reserve(UINT64_MAX, 2) && budget.Used() == 8, "Multiplication overflow must fail before charging");
    valid &= Check(budget.Reserve(2) && budget.Used() == 10 && budget.Peak() == 10, "Exact limit must be admitted");
    valid &= Check(budget.Reserve(0, UINT64_MAX) && !budget.Reserve(1), "Zero bytes remain valid at an exhausted quota");
    budget.Release(10);
    valid &= Check(budget.Used() == 0 && budget.Peak() == 10, "Refund must retain the historical peak");

    GeometryBudget empty(0);
    valid &= Check(empty.Reserve(0) && empty.Reserve(UINT64_MAX, 0) && !empty.Reserve(1) && empty.Peak() == 0,
                   "Zero allowance may not admit any positive bytes");
    const uint64_t addressLimit = static_cast<uint64_t>(SIZE_MAX);
    GeometryBudget maximum(addressLimit);
    valid &= Check(maximum.Reserve(addressLimit) && !maximum.Reserve(1) && maximum.Used() == addressLimit,
                   "Near-maximum addition must not wrap around the quota");
    maximum.Release(addressLimit);
    valid &= Check(maximum.Used() == 0 && maximum.Peak() == addressLimit, "Maximum-size bookkeeping must refund exactly");
    GeometryReservation absent;
    valid &= Check(absent.Reserve(32) && !absent.Reserve(UINT64_MAX, 2),
                   "Unbound legacy reservations still reject impossible byte products");
    return valid;
}

bool Ownership()
{
    GeometryBudget firstBudget(64), secondBudget(16);
    bool           valid = true;
    {
        GeometryReservation first(&firstBudget), destination(&firstBudget);
        valid &= Check(first.Reserve(24) && destination.Reserve(8) && firstBudget.Used() == 32,
                       "Independent owners must share the quota");
        GeometryReservation transferred(std::move(first));
        valid       &= Check(first.Budget() == nullptr && transferred.Budget() == &firstBudget && firstBudget.Used() == 32,
                             "Move construction must transfer a charge without releasing it");
        destination  = std::move(transferred);
        valid       &= Check(transferred.Budget() == nullptr && firstBudget.Used() == 24,
                             "Move assignment must refund only the destination's former charge");
        valid       &= Check(destination.Reserve(8) && firstBudget.Used() == 32,
                             "Moved owner must retain its budget and accept additional reservations");
        destination.Reset(&secondBudget);
        valid &= Check(firstBudget.Used() == 0 && destination.Reserve(16) && secondBudget.Used() == 16,
                       "Reset must refund the previous owner before attaching a new budget");
    }
    valid &= Check(firstBudget.Used() == 0 && secondBudget.Used() == 0, "Every owned charge must be refunded at scope exit");
    return valid;
}

void CompetingWorker(GeometryBudget& budget, std::barrier<>& gate, bool& admitted)
{
    GeometryReservation reservation(&budget);
    gate.arrive_and_wait();
    admitted = reservation.Reserve(80);
    // No worker may refund before every competing request has been resolved.
    gate.arrive_and_wait();
}

bool Competition()
{
    GeometryBudget             budget(128);
    std::barrier<>             gate(4);
    std::array<bool, 4>        admitted{};
    std::array<std::thread, 4> workers;
    for (size_t index = 0; index < workers.size(); ++index)
    {
        workers[index] = std::thread(CompetingWorker, std::ref(budget), std::ref(gate), std::ref(admitted[index]));
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    uint32_t count = 0;
    for (bool accepted : admitted)
    {
        count += accepted ? 1u : 0u;
    }
    return Check(count == 1 && budget.Peak() == 80 && budget.Used() == 0,
                 "Concurrent admission must not overcommit, and every worker must refund after draining");
}

void SharedWorker(GeometryReservation& reservation, std::barrier<>& gate, std::atomic<uint32_t>& failures)
{
    gate.arrive_and_wait();
    for (uint32_t index = 0; index < 4096; ++index)
    {
        if (!reservation.Reserve(1))
        {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool SharedReservation()
{
    GeometryBudget             budget(4 * 4096);
    GeometryReservation        shared(&budget);
    std::barrier<>             gate(4);
    std::atomic<uint32_t>      failures{0};
    std::array<std::thread, 4> workers;
    for (std::thread& worker : workers)
    {
        worker = std::thread(SharedWorker, std::ref(shared), std::ref(gate), std::ref(failures));
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    bool valid = Check(failures.load() == 0 && budget.Used() == 4 * 4096 && budget.Peak() == 4 * 4096,
                       "One shared reservation must retain every concurrent charge");
    shared.Reset();
    valid &= Check(budget.Used() == 0, "Shared reservation may refund only after all writers have drained");
    return valid;
}
} // namespace

int main()
{
    bool valid = Arithmetic();
    valid      = Ownership() && valid;
    valid      = Competition() && valid;
    valid      = SharedReservation() && valid;
    if (valid)
    {
        std::cout << "Geometry quota arithmetic, ownership and concurrent reservation checks passed\n";
    }
    return valid ? 0 : 1;
}
