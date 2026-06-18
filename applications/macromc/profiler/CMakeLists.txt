# macromc_prof — CPU profiler (timers + counters + gauges) for MacroMC.
# Lowest macromc module: depends only on core. Every other macromc module
# links it PUBLIC so worker threads (worldgen, mesh) can record metrics.
add_library(macromc_prof STATIC
    src/profiler.cpp
)
target_include_directories(macromc_prof PUBLIC include)
target_link_libraries(macromc_prof PUBLIC core macromc_public_headers)
