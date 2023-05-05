include(ExternalProject)
include(FetchContent)

FetchContent_Declare(wirecall
    GIT_REPOSITORY https://github.com/jprendes/libwirecall.git
    GIT_TAG        b1fd37916dc8c529ef60868e637b2efc18d86db0
)
FetchContent_MakeAvailable(wirecall)