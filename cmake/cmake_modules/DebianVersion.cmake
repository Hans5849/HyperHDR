# Debian "Version:" for a HyperHDR version string.
#
# In Debian versions "-" starts the Debian revision and "~" sorts before
# anything, including the end of the string. A prerelease therefore maps
# "-" to "~":  24.0.0-beta.0 -> 24.0.0~beta.0, which orders as
#   24.0.0~alpha.1 < 24.0.0~beta.0 < 24.0.0~beta.1 < 24.0.0 < 24.0.1~beta.0
function(HyperhdrDebianVersion OUT VERSION_STRING)
	string(STRIP "${VERSION_STRING}" result)
	string(REPLACE "-" "~" result "${result}")
	if(NOT result MATCHES "^[0-9][A-Za-z0-9.+~]*$")
		message(FATAL_ERROR "Cannot form a valid Debian version from '${VERSION_STRING}'")
	endif()
	set(${OUT} "${result}" PARENT_SCOPE)
endfunction()
