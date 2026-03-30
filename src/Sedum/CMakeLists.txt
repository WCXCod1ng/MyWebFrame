set(LIBRARY_SOURCES
        http/HttpContext.cpp
        http/HttpContext.h
        http/HttpTypes.h
        http/HttpRequest.h
        http/HttpResponse.h
        http/HttpServer.h
        http/HttpServer.cpp
        RouterGroup.cpp
        RouterGroup.h
        WebFrame.cpp
        WebFrame.h
        WebRouter.cpp
        WebRouter.h
        Context.h
        common/Define.h
        common/JsonUtil.h
        pool/mysql_connection_pool.cpp
        common/ObjectPool.h
        common/ConcurrentObjectPool.h
        coroutine/Coroutine.cpp
        coroutine/Coroutine.h
        db/DBInterfaces.h
        db/AsyncMysqlPool.h
        common/LockFreeQueue.h
        common/LockFreeFreeList.h
        common/LockFreeDefines.h
)

add_library(Sedum ${LIBRARY_SOURCES})

# 开启x86_64架构下的cmpxchg16b指令支持
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(Sedum PUBLIC -mcx16)  # Enable cmpxchg16b instruction，注意必须是PUBLIC
endif()

target_include_directories(Sedum
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Sedum
        PUBLIC
        Fleabane
        atomic
)