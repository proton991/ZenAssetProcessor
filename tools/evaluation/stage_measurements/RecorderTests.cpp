#include "Diagnostics/StageMeasurements.h"
#include <iostream>
#include <thread>

namespace
{
using namespace zenasset::detail::measurement;

bool Check(bool valid, const char* message)
{
    if (!valid)
    {
        std::cerr << message << '\n';
    }
    return valid;
}

void Worker(Statistics* own)
{
    // An unbound worker must not touch its caller's binding.
    ZENASSET_MEASURE_SCOPE(MeshDecode);
    ZENASSET_MEASURE_ADD(ReadBytes, 1000);
    if (own != nullptr)
    {
        Binding binding(*own);
        ZENASSET_MEASURE_SCOPE(FileRead);
        ZENASSET_MEASURE_ADD(ReadBytes, 7);
    }
}

bool Partial(bool succeed)
{
    ZENASSET_MEASURE_SCOPE(TransportDecode);
    ZENASSET_MEASURE_ADD(ReadBytes, 3);
    return succeed;
}

bool Verify()
{
    Statistics first, nested, worker;
    {
        Scope unbound(Stage::FileRead);
        Add(Counter::ReadBytes, 1000);
    }
    bool valid = Check(ActiveContext == nullptr, "Unexpected initial binding");
    for (std::string_view name : StageNames)
    {
        valid &= Check(!name.empty(), "Missing stage label");
    }
    valid &= Check(StageNames[size_t(Stage::SourceImages)] == "source_images_io_decode_wait"
                       && StageNames[size_t(Stage::SourceMeshes)] == "source_mesh_convert_wait"
                       && StageNames[size_t(Stage::Checksum)] == "resource_checksum"
                       && StageNames[size_t(Stage::SceneValidate)] == "scene_validate",
                   "Stage names do not match their identities");
    {
        Binding binding(first);
        {
            Scope parent(Stage::Write);
            {
                Scope child(Stage::Serialize);
                Add(Counter::WrittenBytes, 5);
            }
            Scope finalChild(Stage::Publish);
            finalChild.Stop();
            finalChild.Stop();
        }
        {
            Binding nestedBinding(nested);
            Scope   scope(Stage::Read);
            Add(Counter::ReadBytes, 9);
        }
        std::thread unbound(Worker, nullptr);
        std::thread independent(Worker, &worker);
        unbound.join();
        independent.join();
        valid &= Check(!Partial(false), "Failure path changed");
        valid &= Check(Partial(true), "Success path changed");
    }
    const Interval& parent    = first.stages[size_t(Stage::Write)];
    const Interval& serialize = first.stages[size_t(Stage::Serialize)];
    const Interval& publish   = first.stages[size_t(Stage::Publish)];
    valid &= Check(parent.calls == 1 && serialize.calls == 1 && publish.calls == 1, "Wrong nested call counts");
    valid &= Check(parent.inclusiveNs == parent.exclusiveNs + serialize.inclusiveNs + publish.inclusiveNs,
                   "Nested intervals were not subtracted exactly once");
    valid &= Check(serialize.inclusiveNs == serialize.exclusiveNs && publish.inclusiveNs == publish.exclusiveNs,
                   "Leaf intervals differ");
    valid &= Check(first.stages[size_t(Stage::TransportDecode)].calls == 2 && first.counters[size_t(Counter::ReadBytes)] == 6
                       && first.counters[size_t(Counter::WrittenBytes)] == 5,
                   "Partial work or restored binding was lost");
    valid &= Check(first.stages[size_t(Stage::Read)].calls == 0 && first.stages[size_t(Stage::MeshDecode)].calls == 0
                       && nested.stages[size_t(Stage::Read)].calls == 1 && nested.counters[size_t(Counter::ReadBytes)] == 9,
                   "Nested binding mixed records");
    valid &= Check(worker.stages[size_t(Stage::FileRead)].calls == 1 && worker.stages[size_t(Stage::MeshDecode)].calls == 0
                       && worker.counters[size_t(Counter::ReadBytes)] == 7,
                   "Concurrent binding mixed records");
    valid &= Check(ActiveContext == nullptr, "Binding escaped its lifetime");
    return valid;
}
} // namespace

int main()
{
    const bool valid = Verify();
    if (valid)
    {
        std::cout << "Recorder nested, partial, unbound and concurrent-isolation checks passed\n";
    }
    return valid ? 0 : 1;
}
