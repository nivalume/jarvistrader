# Example strategies (docs/plan.md): for tests and soak runs only, never part of the wheel.

add_executable(trade_logger cpp/trade_logger.cpp)
target_link_libraries(trade_logger PRIVATE jarvis::shell)
set_target_properties(trade_logger PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
