include(CMakeFindDependencyMacro)
# Static Processor archives retain link dependencies even though parser types
# are private. Resolve the installed, namespaced packages before importing it.
find_dependency(simdjson 4.6.11 CONFIG)
find_dependency(fastgltf 0.9.1 CONFIG)
find_dependency(draco 1.5.7 CONFIG)
find_dependency(meshoptimizer 1.1 CONFIG)
find_dependency(WebP 1.6.0 CONFIG)
