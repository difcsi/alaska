# Shared Alaska service configuration.
#
# All five Alaska build presets (noservice, anchorage, refcount, refcount-gc,
# refcount-gc-anchorage) include this file via ALASKA_CONFIG. It carries only the
# handle-layout tunables that are identical across every Alaska build; the feature
# triple that distinguishes the presets --
#
#   ALASKA_ENABLE_REFCOUNT / ALASKA_ENABLE_CYCLE_COLLECTION / ALASKA_ENABLE_ANCHORAGE
#
# -- is supplied by each preset's cacheVariables (see CMakePresets.json), NOT here,
# because this file is include()d *after* the alaska_switch() calls in the root
# CMakeLists, which is too late to influence their emitted -D defines.

# The top-level CMakeLists emits -DALASKA_SIZE_BITS from this value (exactly once).
set(ALASKA_SIZE_BITS 32)

# Bits squeezed out of each handle per page (see Configuration.hpp).
set(ALASKA_SQUEEZE_BITS 3)
add_compile_definitions(ALASKA_SQUEEZE_BITS=3)

# Optional debug toggles (all off). To enable one, set it and emit its -D, e.g.
#   add_compile_definitions(ALASKA_SANITY_CHECK=1)
# See runtime/include/alaska/utils.h and the per-feature sources for the guards.
set(ALASKA_SANITY_CHECK FALSE)
set(ALASKA_DUMP_TRANSLATIONS FALSE)
set(ALASKA_DUMP_FLOW_GRAPH FALSE)
set(ALASKA_VERIFY_PASS FALSE)
set(ALASKA_COMPILER_TIMING FALSE)
set(ALASKA_SWAP_SUPPORT FALSE)
set(ALASKA_TRACK_TRANSLATION_HITRATE FALSE)
set(ALASKA_ARGUMENT_TRACE FALSE)
