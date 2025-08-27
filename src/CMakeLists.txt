add_executable(pong
    main.c
    networking.c
    buf.c
)

find_package(raylib)

target_link_libraries(pong PRIVATE raylib project_warnings raygui)
