"""Repository locations shared by the processor's validation tools.

Pure package audits use PROCESSOR_ROOT. Renderer comparisons use ENGINE_ROOT;
set ZENENGINE_SOURCE_DIR when the engine is not a sibling checkout.
"""
import os
from pathlib import Path

VALIDATION_TOOLS = Path(__file__).resolve().parent
PROCESSOR_ROOT = VALIDATION_TOOLS.parents[1]
ENGINE_ROOT = Path(os.environ.get("ZENENGINE_SOURCE_DIR", PROCESSOR_ROOT.parent / "ZenEngine")).resolve()
INTEGRATION_ROOT = PROCESSOR_ROOT / "tests/integration/zenengine"
CORPUS = INTEGRATION_ROOT / "CommonTest/Assets"
