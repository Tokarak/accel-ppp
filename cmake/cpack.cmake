INCLUDE(InstallRequiredSystemLibraries)

SET(CPACK_PACKAGE_VERSION_MAJOR "1")
SET(CPACK_PACKAGE_VERSION_MINOR "3")
SET(CPACK_PACKAGE_VERSION_PATCH "7")

SET(CPACK_PACKAGE_NAME "accel-ppp")
SET(CPACK_PACKAGE_CONTACT "Dmitry Kozlov <xeb@mail.ru>")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PPtP/L2TP/PPPoE server for Linux")

SET(CPACK_PACKAGE_VENDOR "Dmitry Kozlov")
SET(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README")
SET(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")

IF(CPACK_TYPE STREQUAL Debian5)
	SET(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.7), libssl0.9.8 (>= 0.9.8), libpcre3 (>= 7.6)")
	INCLUDE(${CMAKE_HOME_DIRECTORY}/cmake/debian/debian.cmake)
ENDIF(CPACK_TYPE STREQUAL Debian5)

IF(CPACK_TYPE STREQUAL Debian6)
	SET(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.11.2), libssl0.9.8 (>= 0.9.8), libpcre3 (>= 8.02), libnl2 (>= 1.99)")
	INCLUDE(${CMAKE_HOME_DIRECTORY}/cmake/debian/debian.cmake)
ENDIF(CPACK_TYPE STREQUAL Debian6)

INCLUDE(CPack)
