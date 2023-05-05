include(ExternalProject)
include(FetchContent)

FetchContent_Declare(wirecall
    GIT_REPOSITORY https://github.com/jprendes/libwirecall.git
    GIT_TAG        747ab6d0b64ae56c17943f60bdc1de99f9cc6bea
)
FetchContent_MakeAvailable(wirecall)