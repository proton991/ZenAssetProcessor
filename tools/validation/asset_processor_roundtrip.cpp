// Source-free package read/write probe for independent payload audits.
#include "ZenAsset/Processor/Write.h"
#include <cstdio>

int main(int argc, char** argv)
{
    zenasset::Status status;
    if (argc != 3)
    {
        status = {zenasset::StatusCode::InvalidArgument, "Expected input and output package paths"};
    }
    else
    {
        const zenasset::SceneReadResult loaded = zenasset::ReadScene(argv[1]);
        status                                 = loaded.status;
        if (status.Succeeded())
        {
            zenasset::SceneWriteOptions options;
            options.metadata = loaded.metadata;
            status           = zenasset::WriteScene(argv[2], *loaded.scene, options);
        }
    }
    if (!status.Succeeded())
    {
        std::fprintf(stderr, "%s\n", status.message.c_str());
    }
    return status.Succeeded() ? 0 : 1;
}
