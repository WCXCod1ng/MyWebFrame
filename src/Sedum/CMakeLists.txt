set(LIBRARY_SOURCES
        WebFrame.cpp
        WebRouter.cpp
        pool/mysql_connection_pool.cpp
)

add_library(Sedum ${LIBRARY_SOURCES})

target_include_directories(Sedum
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Sedum
        PUBLIC
        Fleabane
)