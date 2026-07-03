
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -gdwarf-4 -fdeclspec ${WARNINGS} ")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -gdwarf-4 -fdeclspec ${WARNINGS} -std=c++17 -Wno-deprecated -fno-exceptions -fno-rtti -Wno-unused-command-line-argument ")

include_directories(
	${LLVM_INCLUDE_DIRS}
	include/
)
# add_definitions(${LLVM_DEFINITIONS})

# Reference-counting instrumentation passes are only compiled into the plugin
# when the feature is enabled (see ALASKA_ENABLE_REFCOUNT).
if(ALASKA_ENABLE_REFCOUNT)
  set(ALASKA_REFCOUNT_PASS_SOURCES
    passes/RefcountInc.cpp
    passes/RefcountDec.cpp)
else()
  set(ALASKA_REFCOUNT_PASS_SOURCES "")
endif()

install(DIRECTORY ldscripts DESTINATION share/alaska)
# set(LLVM_LINK_COMPONENTS Linker)


add_llvm_library(Alaska MODULE
	passes/Alaska.cpp

	lib/PointerFlowGraph.cpp
	lib/EscapeAnalysis.cpp
	lib/Utils.cpp
	lib/TranslationForest.cpp
	lib/Translations.cpp
	lib/OptimisticTypes.cpp
	lib/TypedPointer.cpp
  lib/Types.cpp
  lib/TypeContext.cpp
  lib/TypeInference.cpp
  lib/AccessAutomata.cpp

	
	# ======= Hacky Noelle Embedding ======= 
	lib/noelle/MetadataEntry.cpp
	lib/noelle/MetadataManager_Instruction.cpp
	lib/noelle/MetadataManager_Module.cpp

	lib/noelle/DataFlowAnalysis.cpp
	lib/noelle/DataFlowEngine.cpp
	lib/noelle/DataFlowResult.cpp

	# lib/noelle/BitMatrix.cpp
	# lib/noelle/CallGraph.cpp
	# lib/noelle/CallGraphEdge.cpp
	# lib/noelle/CallGraphNode.cpp
	# lib/noelle/CallGraphTraits.cpp
	# lib/noelle/IntegrationWithSVF.cpp
	# lib/noelle/PDGAnalysis_callGraph.cpp
	# lib/noelle/PDGAnalysis_compare.cpp
	# lib/noelle/PDGAnalysis_controlDependences.cpp
	# lib/noelle/PDGAnalysis.cpp
	# lib/noelle/PDGAnalysis_embedder.cpp
	# lib/noelle/PDGAnalysis_memory.cpp
	# lib/noelle/PDG.cpp
	# lib/noelle/PDGPrinter.cpp
	# lib/noelle/SCCCAG.cpp
	# lib/noelle/SCCCAGNode.cpp
	# lib/noelle/SCCCAGNode_Function.cpp
	# lib/noelle/SCCCAGNode_SCC.cpp
	# lib/noelle/SCC.cpp
	# lib/noelle/SCCDAG.cpp
	# lib/noelle/SubCFGs.cpp
	# lib/noelle/Utils.cpp
	# lib/noelle/AllocAA.cpp
	# lib/noelle/MemorySite.cpp
	# ======= Hacky Noelle Embedding ======= 

	passes/PinTracking.cpp
	passes/TranslationPrinter.cpp
	passes/ArgPinElision.cpp
	passes/Normalize.cpp
	passes/Escape.cpp
	passes/StackPromote.cpp
	passes/HoistInductionTranslate.cpp
	passes/TranslatePass.cpp
	passes/Replacement.cpp
	passes/Lower.cpp
	passes/PlaceSafepoints.cpp
 	passes/ArgumentTrace.cpp
	${ALASKA_REFCOUNT_PASS_SOURCES}

	PLUGIN_TOOL
	opt
)

# Match the ABI used by the available LLVM runtime in this environment.
target_compile_options(Alaska PRIVATE -stdlib=libstdc++)
target_link_options(Alaska PRIVATE -stdlib=libstdc++)
target_compile_options(Alaska PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>)
target_include_directories(Alaska SYSTEM PRIVATE
	/usr/include/c++/13
	/usr/include/x86_64-linux-gnu/c++/13
	/usr/include/c++/13/backward)

# Keep plugin loading model self-contained: resolve LLVM symbols from the
# hosting `opt` process instead of depending on an external libLLVM*.so.
set_property(TARGET Alaska PROPERTY LINK_FLAGS "-Wl,--allow-shlib-undefined")

add_subdirectory(bin)

