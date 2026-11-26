add_executable(
    AirPodsDomainTests

    AirPodsDomainTests.cpp
)

target_link_libraries(
    AirPodsDomainTests

    PRIVATE
    apd_core
    apd_presentation
    Qt6::Test
)

target_include_directories(AirPodsDomainTests PRIVATE "${CMAKE_SOURCE_DIR}")

# The qrc is linked into the application target, so resource checks read the source tree instead.
target_compile_definitions(
    AirPodsDomainTests PRIVATE APD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME AirPodsDomainTests COMMAND AirPodsDomainTests)

add_executable(QuickConnectTests QuickConnectTests.cpp)

target_link_libraries(
    QuickConnectTests

    PRIVATE
    apd_core
    Qt6::Test
    Qt6::Widgets
)

target_include_directories(QuickConnectTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME QuickConnectTests COMMAND QuickConnectTests)

add_executable(AAPProtocolTests AAPProtocolTests.cpp)

target_link_libraries(
    AAPProtocolTests

    PRIVATE
    apd_core
    Qt6::Test
)

target_include_directories(AAPProtocolTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME AAPProtocolTests COMMAND AAPProtocolTests)

add_executable(MagicAAPTests MagicAAPTests.cpp)

target_link_libraries(
    MagicAAPTests

    PRIVATE
    apd_core
    Qt6::Test
    windowsapp.lib
    runtimeobject.lib
)

target_include_directories(MagicAAPTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME MagicAAPTests COMMAND MagicAAPTests)

add_executable(AAPConnectionStateMachineTests AAPConnectionStateMachineTests.cpp)

target_link_libraries(
    AAPConnectionStateMachineTests

    PRIVATE
    apd_domain
    Qt6::Test
)

target_include_directories(AAPConnectionStateMachineTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME AAPConnectionStateMachineTests COMMAND AAPConnectionStateMachineTests)

add_executable(AAPTransportTests AAPTransportTests.cpp)

target_link_libraries(
    AAPTransportTests

    PRIVATE
    apd_core
    Qt6::Test
)

target_include_directories(AAPTransportTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME AAPTransportTests COMMAND AAPTransportTests)

add_executable(AAPManagerWorkerTests AAPManagerWorkerTests.cpp)

target_link_libraries(
    AAPManagerWorkerTests

    PRIVATE
    apd_core
    Qt6::Test
)

target_include_directories(AAPManagerWorkerTests PRIVATE "${CMAKE_SOURCE_DIR}")

add_test(NAME AAPManagerWorkerTests COMMAND AAPManagerWorkerTests)

add_executable(AnimationViewTests AnimationViewTests.cpp)

target_link_libraries(
    AnimationViewTests

    PRIVATE
    apd_gui
    Qt6::Multimedia
    Qt6::Test
    Qt6::Widgets
)

target_include_directories(AnimationViewTests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(
    AnimationViewTests
    PRIVATE
    APD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    APD_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

add_test(NAME AnimationViewTests COMMAND AnimationViewTests)

add_executable(UiRenderingTests UiRenderingTests.cpp ${CMAKE_SOURCE_DIR}/Source/Resource/Resource.qrc)

target_link_libraries(
    UiRenderingTests

    PRIVATE
    apd_gui
    Qt6::Test
    Qt6::Widgets
)

target_include_directories(UiRenderingTests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(UiRenderingTests PRIVATE APD_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_dependencies(UiRenderingTests apd_translations apd_animations)

add_test(NAME UiRenderingTests COMMAND UiRenderingTests)
foreach(scale_factor 1 1.25 1.5 2)
    string(REPLACE "." "_" scale_suffix "${scale_factor}")
    add_test(
        NAME MainWindowRenderingScale${scale_suffix}
        COMMAND UiRenderingTests MainWindowElidesLongDeviceNames
    )
    set_tests_properties(
        MainWindowRenderingScale${scale_suffix}
        PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=${scale_factor}"
    )
    add_test(
        NAME AirPods5RenderingScale${scale_suffix}
        COMMAND UiRenderingTests MainWindowRendersAirPods5
    )
    set_tests_properties(
        AirPods5RenderingScale${scale_suffix}
        PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=${scale_factor}"
    )
    add_test(
        NAME AirPodsMaxRenderingScale${scale_suffix}
        COMMAND UiRenderingTests MainWindowShowsTheHeadsetAtFullSize MainWindowShowsOnlyOneMaxBattery
    )
    set_tests_properties(
        AirPodsMaxRenderingScale${scale_suffix}
        PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=${scale_factor}"
    )
    add_test(
        NAME NotificationRenderingScale${scale_suffix}
        COMMAND UiRenderingTests NotificationCardSharesTheTitleRowWithClose
    )
    set_tests_properties(
        NotificationRenderingScale${scale_suffix}
        PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=${scale_factor}"
    )
endforeach()

foreach(test_target
        AirPodsDomainTests
        QuickConnectTests
        AAPProtocolTests
        MagicAAPTests
        AAPConnectionStateMachineTests
        AAPTransportTests
        AAPManagerWorkerTests
        AnimationViewTests
        UiRenderingTests)
    add_custom_command(
        TARGET ${test_target}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Test>
            $<TARGET_FILE_DIR:${test_target}>
        COMMENT "Deploying Qt Test runtime for ${test_target}"
    )
endforeach()

# Every registered test gets a hard 60 s ceiling. The blocking AAP transport and worker
# suites are designed to release their own parked threads, but a regression that leaves a
# worker parked must surface as one failed test rather than as a stalled CI job. Reading
# the TESTS directory property means a newly registered target can never be skipped.
get_property(apd_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${apd_registered_tests} PROPERTIES TIMEOUT 60)
