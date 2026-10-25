#include "Diagnostics/StageMeasurements.h"

int main()
{
    int effects = 0;
    // Undefined tokens are intentional: disabled macros must not instantiate
    // a timer, require stage types or evaluate their arguments.
    ZENASSET_MEASURE_BEGIN(undefinedScope, UndefinedStage);
    ZENASSET_MEASURE_SCOPE(UndefinedStage);
    ZENASSET_MEASURE_ADD(UndefinedCounter, ++effects);
    ZENASSET_MEASURE_ADD(UndefinedCounter, UndefinedFunction());
    ZENASSET_MEASURE_END(undefinedScope);
    return effects;
}
