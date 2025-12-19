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