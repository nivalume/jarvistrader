# jarvis_add_strategy_plugin(<target> <sources>...)
#
# A shared library of native strategies that a node loads at run time (jarvis.load_native in
# Python; docs/architecture.md section 7.3). Its sources register strategies with
# JARVIS_REGISTER_STRATEGY and one of them adds JARVIS_STRATEGY_PLUGIN(). Only that entry point
# is exported: the plugin's own copies of the jarvis code stay private to it. Build the plugin
# from the same jarvis sources, compiler and flags as the node that loads it.
function(jarvis_add_strategy_plugin target)
  add_library(${target} MODULE ${ARGN})
  target_link_libraries(${target} PRIVATE jarvis::shell)
  set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden
                                             VISIBILITY_INLINES_HIDDEN ON PREFIX "lib")
  if(NOT APPLE)
    target_link_options(${target} PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
endfunction()
