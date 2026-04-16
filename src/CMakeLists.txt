add_library(Asterisk
    utils/circuit.cpp
    utils/types.cpp
    utils/helpers.cpp
    Asterisk2.0/key_manager.cpp
    Asterisk2.0/mac_setup.cpp
    Asterisk2.0/app_evaluator.cpp
    Asterisk2.0/protocol.cpp
    asterisk/sharing.cpp
    asterisk/rand_gen_pool.cpp
    asterisk/offline_evaluator.cpp
    asterisk/online_evaluator_load_balanced.cpp
    assistedMPC/sharing.cpp
    assistedMPC/rand_gen_pool.cpp
    assistedMPC/offline_evaluator.cpp)

target_include_directories(Asterisk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Asterisk PUBLIC Boost::system EMPTool NTL GMP)
