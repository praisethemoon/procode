# Removes every file listed in the build tree's install_manifest.txt
# (written by `cmake --install`). Invoked by the `uninstall` target.
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR
            "no install manifest at ${MANIFEST}; run cmake --install first")
endif()

file(STRINGS "${MANIFEST}" installed_files)
foreach(f IN LISTS installed_files)
    if(EXISTS "${f}" OR IS_SYMLINK "${f}")
        message(STATUS "removing ${f}")
        file(REMOVE "${f}")
        if(EXISTS "${f}")
            message(WARNING "could not remove ${f}")
        endif()
    else()
        message(STATUS "already gone: ${f}")
    endif()
endforeach()
