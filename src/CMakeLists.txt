cmake_minimum_required ( VERSION 3.17 )

FOREACH ( policy CMP0054 CMP0057 CMP0042 CMP0074 CMP0110 )
	IF ( POLICY ${policy} )
		CMAKE_POLICY ( SET ${policy} NEW )
	ENDIF ()
ENDFOREACH ()

message(STATUS "Inside directory src...")

#set (CMAKE_DEBUG_TARGET_PROPERTIES
#		INCLUDE_DIRECTORIES
#		COMPILE_DEFINITIONS
#		LINK_LIBRARIES
#		POSITION_INDEPENDENT_CODE
#		CONTAINER_SIZE_REQUIRED
#		LIB_VERSION
#		)

include (GetColumnar)
target_link_libraries (lextra INTERFACE columnar::columnar_api)
target_include_directories ( lextra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} )

get_banner (CONFIGURE_FLAGS)
infomsg ("${CONFIGURE_FLAGS}")

infomsg("Generating config files...")
configure_file("${MANTICORE_SOURCE_DIR}/config/config_cmake.h.in"
		"${MANTICORE_BINARY_DIR}/config/config_.h" ESCAPE_QUOTES)
configure_file ("${MANTICORE_SOURCE_DIR}/config/config_indexer_cmake.h.in"
		"${MANTICORE_BINARY_DIR}/config/config_indexer_.h" ESCAPE_QUOTES)

# avoid config rebuild if configuration produces the same file
execute_process (COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MANTICORE_BINARY_DIR}/config/config_.h" "${MANTICORE_BINARY_DIR}/config/config.h")
execute_process (COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MANTICORE_BINARY_DIR}/config/config_indexer_.h" "${MANTICORE_BINARY_DIR}/config/config_indexer.h")

add_library (generated_config INTERFACE)
target_include_directories(generated_config INTERFACE "${MANTICORE_BINARY_DIR}/config")
target_link_libraries (lextra INTERFACE generated_config)

# main bunch of sources (for libsphinx)
add_library(lmanticore STATIC sphinx.cpp sphinxexcerpt.cpp sphinxquery.cpp sphinxsoundex.cpp sphinxmetaphone.cpp
		sphinxstemen.cpp sphinxstemru.cpp sphinxstemru.inl sphinxstemcz.cpp sphinxstemar.cpp sphinxutils.cpp
		sphinxstd.cpp sphinxsort.cpp sortsetup.cpp sphinxexpr.cpp sphinxfilter.cpp sphinxsearch.cpp sphinxrt.cpp
		sphinxjson.cpp sphinxaot.cpp sphinxplugin.cpp sphinxudf.c sphinxqcache.cpp sphinxjsonquery.cpp
		jsonqueryfilter.cpp attribute.cpp secondaryindex.cpp killlist.cpp searchnode.cpp json/cJSON.c sphinxpq.cpp
		global_idf.cpp docstore.cpp lz4/lz4.c lz4/lz4hc.c searchdexpr.cpp snippetfunctor.cpp snippetindex.cpp
		snippetstream.cpp snippetpassage.cpp threadutils.cpp sphinxversion.cpp indexcheck.cpp datareader.cpp
		indexformat.cpp indexsettings.cpp fileutils.cpp threads_detached.cpp hazard_pointer.cpp
		task_info.cpp mini_timer.cpp fileio.cpp memio.cpp queryprofile.cpp columnarfilter.cpp columnargrouper.cpp
		columnarlib.cpp collation.cpp fnv64.cpp histogram.cpp threads_detached.cpp hazard_pointer.cpp
		mini_timer.cpp dynamic_idx.cpp columnarrt.cpp columnarmisc.cpp exprtraits.cpp columnarexpr.cpp
		sphinx_alter.cpp columnarsort.cpp binlog.cpp chunksearchctx.cpp client_task_info.cpp
		indexfiles.cpp attrindex_builder.cpp queryfilter.cpp aggregate.cpp)

add_library ( conversion conversion.cpp )
target_link_libraries ( conversion PUBLIC lextra )
target_link_libraries ( lmanticore PUBLIC conversion )

add_subdirectory ( stripper )
target_link_libraries ( lmanticore PUBLIC stripper )

add_subdirectory ( tokenizer )
target_link_libraries ( lmanticore PUBLIC tokenizer )
target_link_libraries ( lmanticore PUBLIC lextra )

add_subdirectory ( schema )
target_link_libraries ( lmanticore PUBLIC schema )

add_library(lmanticore_icu icu.cpp)
if (WITH_ICU)
	target_link_libraries (lmanticore_icu PRIVATE icu::icu)
endif()
target_link_libraries (lmanticore_icu PRIVATE lextra)
target_link_libraries (lmanticore PUBLIC lmanticore_icu)

if (WITH_STEMMER)
	target_link_libraries (lmanticore PRIVATE stemmer::stemmer)
endif ()
if (WITH_RE2)
	target_link_libraries (lmanticore PRIVATE re2::re2)
endif ()

add_subdirectory ( indexing_sources )
target_link_libraries ( lmanticore PUBLIC sourcedoc )

if (WIN32)
	target_sources (lmanticore PRIVATE libutils.cpp)
	target_compile_options(lmanticore PRIVATE /bigobj)
endif()

# Boost is required dependency now
set (Boost_USE_MULTITHREADED ON)
set (Boost_USE_STATIC_LIBS ON)
find_package (Boost 1.61.0 REQUIRED COMPONENTS context)
target_link_libraries ( lextra INTERFACE Boost::boost Boost::context )

# Fixme! That is temporary solution - as we just use some headers of Boost::fiber, and that is available by default.
# If we link with the lib, we should find_package for component fiber, and then link with it. *NOT* that is not necessary.
# But Windows implicitly links with boost::fibers even if we just include some of that headers. That is why BOOST_ALL_NO_LIB
# is poisoned into lextra interface - to avoid such behavior.
# (we don't link with fiber since our custom build dockers doesn't include it, and they need to be rebuilt before)
target_compile_definitions (lextra INTERFACE BOOST_ALL_NO_LIB=1)

add_library (lmanticore_coro coroutine.cpp coroutine.h coro_waker.h)
target_link_libraries(lmanticore_coro PUBLIC lextra )

find_package (Valgrind QUIET)
if (Valgrind_FOUND)
	target_link_libraries (lmanticore_coro PRIVATE Valgrind::Valgrind)
	target_compile_definitions (lmanticore_coro PRIVATE HAVE_VALGRIND=1)
endif()

target_link_libraries (lmanticore PUBLIC lmanticore_coro)

set(LMANTICORE_BISON sphinxexpr.y sphinxselect.y sphinxquery.y sphinxjson.y)
set(LMANTICORE_FLEX sphinxjson.l sphinxexpr.l)
set(SEARCHD_BISON sphinxql.y sphinxql_debug.y ddl.y)
set(SEARCHD_FLEX sphinxql.l sphinxql_debug.l ddl.l)

# The headers is not neccessary for executable build, but nesessary for MSVC and other projects
# all the (non-generated) headers
# So if you add headers to the project and NOT see them in visual studio solution - just list them here!
set ( HEADERS sphinxexcerpt.h sphinxfilter.h sphinxint.h sphinxjsonquery.h sphinxpq.h sphinxql_debug.h sphinxrt.h
		sphinxsort.h sphinxstem.h sphinxutils.h sphinxexpr.h sphinx.h sphinxjson.h sphinxplugin.h sphinxqcache.h
		sphinxquery.h sphinxsearch.h sphinxstd.h sphinxudf.h lz4/lz4.h lz4/lz4hc.h http/http_parser.h secondaryindex.h
		searchnode.h killlist.h attribute.h accumulator.h global_idf.h optional.h event.h threadutils.h threadutils_impl.h
		hazard_pointer.h task_info.h mini_timer.h collation.h fnv64.h histogram.h sortsetup.h dynamic_idx.h
		indexsettings.h columnarlib.h fileio.h memio.h queryprofile.h columnarfilter.h columnargrouper.h fileutils.h
		libutils.h conversion.h columnarsort.h sortcomp.h binlog_defs.h binlog.h ${MANTICORE_BINARY_DIR}/config/config.h
		chunksearchctx.h lrucache.h indexfiles.h attrindex_builder.h queryfilter.h aggregate.h)

set ( SEARCHD_H searchdaemon.h searchdconfig.h searchdddl.h searchdexpr.h searchdha.h searchdreplication.h searchdsql.h
		searchdtask.h client_task_info.h taskflushattrs.h taskflushbinlog.h taskflushmutable.h taskglobalidf.h
		taskmalloctrim.h taskoptimize.h taskping.h taskpreread.h tasksavestate.h net_action_accept.h
		netreceive_api.h netreceive_http.h netreceive_ql.h netstate_api.h networking_daemon.h optional.h query_status.h
		compressed_zlib_mysql.h sphinxql_debug.h stackmock.h replication/wsrep_api_stub.h searchdssl.h digest_sha1.h
		client_session.h compressed_zstd_mysql.h)

source_group ( "Grammar sources" FILES ${LMANTICORE_BISON} ${SEARCHD_BISON} )
source_group ( "Lexer sources" FILES ${LMANTICORE_FLEX} ${SEARCHD_FLEX} )

# add the extra targets in the case we want on-the-fly grammar compiler
find_package (BISON REQUIRED)
set (BISON_DIR "${MANTICORE_BINARY_DIR}/config")
set (BIS_FLAGS "")
if (BISON_VERSION VERSION_GREATER 3.0)
	set (BIS_FLAGS "-Wno-deprecated")
endif ()

BISON_TARGET (ExprParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxexpr.y" ${BISON_DIR}/bissphinxexpr.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (SelectParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxselect.y" ${BISON_DIR}/bissphinxselect.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (QueryParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxquery.y" ${BISON_DIR}/bissphinxquery.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (SQLParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxql.y" ${BISON_DIR}/bissphinxql.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (DDLParser "${CMAKE_CURRENT_SOURCE_DIR}/ddl.y" ${BISON_DIR}/bisddl.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (JsonParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxjson.y" ${BISON_DIR}/bissphinxjson.c COMPILE_FLAGS ${BIS_FLAGS})
BISON_TARGET (SQLDebugParser "${CMAKE_CURRENT_SOURCE_DIR}/sphinxql_debug.y" ${BISON_DIR}/bissphinxql_debug.c COMPILE_FLAGS ${BIS_FLAGS})

set_property (SOURCE sphinxexpr.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_ExprParser_OUTPUT_SOURCE})
set_property (SOURCE sphinx.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_SelectParser_OUTPUT_SOURCE})
set_property (SOURCE sphinxquery.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_QueryParser_OUTPUT_SOURCE})
set_property (SOURCE sphinxjson.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_JsonParser_OUTPUT_SOURCE})
set_property (SOURCE searchdddl.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_DDLParser_OUTPUT_SOURCE})
set_property (SOURCE searchdsql.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_SQLParser_OUTPUT_SOURCE})
set_property (SOURCE sphinxql_debug.cpp APPEND PROPERTY OBJECT_DEPENDS ${BISON_SQLDebugParser_OUTPUT_SOURCE})

# add the extra targets in the case we want on-the-fly lex compiler
find_package (FLEX REQUIRED)
set (FLEX_DIR "${MANTICORE_BINARY_DIR}/config")
FLEX_TARGET (SQLlex "${CMAKE_CURRENT_SOURCE_DIR}/sphinxql.l" ${FLEX_DIR}/flexsphinxql.c)
FLEX_TARGET (Jsonlex "${CMAKE_CURRENT_SOURCE_DIR}/sphinxjson.l" ${FLEX_DIR}/flexsphinxjson.c)
FLEX_TARGET (Exprlex "${CMAKE_CURRENT_SOURCE_DIR}/sphinxexpr.l" ${FLEX_DIR}/flexsphinxexpr.c)
FLEX_TARGET (DDLlex "${CMAKE_CURRENT_SOURCE_DIR}/ddl.l" ${FLEX_DIR}/flexddl.c)
FLEX_TARGET (SQLDebuglex "${CMAKE_CURRENT_SOURCE_DIR}/sphinxql_debug.l" ${FLEX_DIR}/flexsphinxqldebug.c)

set_property (SOURCE searchdsql.cpp APPEND PROPERTY OBJECT_DEPENDS ${FLEX_SQLlex_OUTPUTS})
set_property (SOURCE searchdddl.cpp APPEND PROPERTY OBJECT_DEPENDS ${FLEX_DDLlex_OUTPUTS})
set_property (SOURCE sphinxjson.cpp APPEND PROPERTY OBJECT_DEPENDS ${FLEX_Jsonlex_OUTPUTS})
set_property (SOURCE sphinxexpr.cpp APPEND PROPERTY OBJECT_DEPENDS ${FLEX_Exprlex_OUTPUTS})
set_property (SOURCE sphinxql_debug.cpp APPEND PROPERTY OBJECT_DEPENDS ${FLEX_SQLDebuglex_OUTPUTS})

add_custom_target (
		vergen
		COMMAND
		${CMAKE_COMMAND}
		-D SOURCE_DIR=${MANTICORE_SOURCE_DIR}
		-D BINARY_DIR=${MANTICORE_BINARY_DIR}
		-D BUILD_TAG=${BUILD_TAG}
		-D CPACK_SUFFIX=${CPACK_SUFFIX}
		-P ${MANTICORE_SOURCE_DIR}/sphinxrev.cmake
		COMMENT "Generate version from sources"
		VERBATIM
		SOURCES ${MANTICORE_SOURCE_DIR}/sphinxrev.cmake
		${MANTICORE_SOURCE_DIR}/src/sphinxversion.h.in
		${MANTICORE_SOURCE_DIR}/dist/CPackOptions.cmake.in
)
add_dependencies (lextra vergen)

# our mega-lib
target_sources(lmanticore PUBLIC ${LMANTICORE_BISON} ${LMANTICORE_FLEX} ${HEADERS} ${CHARSET_FILES} ${CHARSET_TEMPLATE} "../misc/manticore.natvis")

add_library (lsearchd OBJECT searchdha.cpp http/http_parser.c searchdhttp.cpp
		searchdtask.cpp taskping.cpp taskmalloctrim.cpp taskoptimize.cpp taskglobalidf.cpp tasksavestate.cpp
		taskflushbinlog.cpp taskflushattrs.cpp taskflushmutable.cpp taskpreread.cpp
		searchdaemon.cpp searchdfields.cpp searchdconfig.cpp
		searchdsql.cpp searchdddl.cpp networking_daemon.cpp
		netstate_api.cpp net_action_accept.cpp netreceive_api.cpp
		netreceive_http.cpp netreceive_ql.cpp query_status.cpp
		sphinxql_debug.cpp stackmock.cpp )
target_sources (lsearchd PUBLIC ${SEARCHD_SRCS_TESTABLE} ${SEARCHD_H} ${SEARCHD_BISON} ${SEARCHD_FLEX})
add_library ( digest_sha1 digest_sha1.cpp )
target_link_libraries ( digest_sha1 PRIVATE lextra )
target_link_libraries ( lsearchd PUBLIC digest_sha1 lextra )

diag ( CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION )
if (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
#		if (CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "12")
		if (CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "12.0.1" OR CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "12")
			diags("Select precalculated values for Clang 12.0.X on x86_64" )
			set_source_files_properties ( stackmock.cpp PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "KNOWN_CREATE_SIZE=768;KNOWN_EXPR_SIZE=32;KNOWN_FILTER_SIZE=208" )
			set_source_files_properties ( stackmock.cpp PROPERTIES COMPILE_DEFINITIONS_DEBUG "KNOWN_CREATE_SIZE=4208;KNOWN_EXPR_SIZE=48;KNOWN_FILTER_SIZE=400")
		endif()
	elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		if (CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "9.3")
			diags ( "Select precalculated values for GCC 9.3 on x86_64" )
			set_source_files_properties ( stackmock.cpp PROPERTIES COMPILE_DEFINITIONS_RELWITHDEBINFO "KNOWN_CREATE_SIZE=16;KNOWN_EXPR_SIZE=48;KNOWN_FILTER_SIZE=240" )
			set_source_files_properties ( stackmock.cpp PROPERTIES COMPILE_DEFINITIONS_DEBUG "KNOWN_CREATE_SIZE=256;KNOWN_EXPR_SIZE=48;KNOWN_FILTER_SIZE=272" )
		endif()
	endif ()
endif()

if (WITH_ZLIB OR WITH_ZSTD)
	target_sources ( lsearchd PRIVATE compressed_mysql_layer.cpp compressed_mysql_layer.h)
	if (WITH_ZLIB)
		target_sources ( lsearchd PRIVATE compressed_zlib_mysql.cpp )
		target_link_libraries (lsearchd PRIVATE ZLIB::ZLIB)
	endif()

	if (WITH_ZSTD)
		target_sources ( lsearchd PRIVATE compressed_zstd_mysql.cpp )
		if (DL_ZSTD)
			target_link_libraries ( lsearchd PRIVATE ZSTD::ZSTD_ld )
		else ()
			target_link_libraries ( lsearchd PRIVATE ZSTD::ZSTD )
		endif ()
	endif ()
endif ()

# stub library for all except daemon - to avoid link with openssl
add_library (stub_ssl searchdssl.cpp)
target_link_libraries (stub_ssl PRIVATE lextra)

# support for ssl. Actual only for physical daemon
add_library (searchd_ssl searchdssl.cpp)
target_link_libraries (searchd_ssl PUBLIC lextra)
if (WITH_SSL)
	target_compile_definitions (searchd_ssl PRIVATE DAEMON)
	target_link_libraries (searchd_ssl PRIVATE OpenSSL::SSL)
	target_link_libraries (digest_sha1 PRIVATE OpenSSL::SSL)
	target_compile_options (searchd_ssl PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-deprecated-declarations>")
	include(CheckFunctionExists)
	set(CMAKE_REQUIRED_LIBRARIES "OpenSSL::SSL")
	check_function_exists(TLS_server_method HAVE_TLS_SERVER_METHOD)
	if (HAVE_TLS_SERVER_METHOD)
		target_compile_definitions(searchd_ssl PRIVATE HAVE_TLS_SERVER_METHOD=${HAVE_TLS_SERVER_METHOD})
	endif()
	check_function_exists(TLSv1_2_method HAVE_TLSV1_2_METHOD)
	if (HAVE_TLSV1_2_METHOD)
		target_compile_definitions(searchd_ssl PRIVATE HAVE_TLSV1_2_METHOD=${HAVE_TLSV1_2_METHOD})
	endif()
	check_function_exists(TLSv1_1_server_method HAVE_TLSV1_1_SERVER_METHOD)
	if (HAVE_TLSV1_1_SERVER_METHOD)
		target_compile_definitions(searchd_ssl PRIVATE HAVE_TLSV1_1_SERVER_METHOD=${HAVE_TLSV1_1_SERVER_METHOD})
	endif()
	if (WIN32 AND TARGET OpenSSL::applink)
		target_link_libraries (searchd_ssl PRIVATE OpenSSL::applink)
		target_compile_options ( OpenSSL::applink INTERFACE $<${CLANG_CL}:-Wno-pedantic -Wno-missing-prototypes> )
	endif()
endif (WITH_SSL)

# support for replication
add_library(searchd_replication searchdreplication.cpp)
target_link_libraries(searchd_replication PUBLIC lextra)
if (WITH_GALERA)
	add_subdirectory(replication)
	target_link_libraries(searchd_replication PRIVATE manticore::wsrep)
	install_galera ("${CMAKE_INSTALL_DATADIR}/manticore/modules")
endif ()
target_link_libraries(lsearchd PUBLIC searchd_replication)

trace(lextra)
trace(lsearchd)

if (SPLIT_SYMBOLS)
	include (split)
endif()

# our executables
add_executable(searchd searchd.cpp)
target_link_libraries (searchd searchd_ssl lmanticore lsearchd)

if (NOT STATIC_BINARY)
	add_executable(indexer indexer.cpp)
	add_executable(indextool indextool.cpp)
	add_executable(spelldump spelldump.cpp)
	add_executable(wordbreaker wordbreaker.cpp)
	add_executable(index_converter index_converter.cpp)
	target_link_libraries(indexer indexer_sources lmanticore)
	target_link_libraries(indextool lmanticore)
	target_link_libraries(spelldump lmanticore)
	target_link_libraries(wordbreaker lmanticore)
	target_link_libraries(index_converter lmanticore)
	add_library(udfexample MODULE udfexample.c)
	target_compile_options (udfexample PRIVATE "$<${CLANG_CL}:-Wno-deprecated-declarations>")
endif ()

if ( SPLIT_SYMBOLS )
	split_dbg ( indexer )
	split_dbg ( indextool )
	split_dbg ( searchd )
	split_dbg ( spelldump )
	split_dbg ( wordbreaker )
	split_dbg ( index_converter )
endif()

INSTALL (TARGETS searchd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT applications)
if (NOT STATIC_BINARY)
	INSTALL (TARGETS indexer indextool spelldump wordbreaker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tools)
	INSTALL (TARGETS index_converter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT converter)
	INSTALL (FILES sphinxudf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/manticore COMPONENT devel)
endif ()

# install windows runtime libraries - for final targets, and also for immediate testing
if (WIN32)
	set (CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
	include (InstallRequiredSystemLibraries)
	addruntime ("${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
	getruntime (libs)
	install (PROGRAMS ${libs} DESTINATION bin COMPONENT runtime)
	if (BUILD_TESTING)
		add_custom_command (TARGET searchd POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy ${libs} "$<TARGET_FILE_DIR:searchd>" VERBATIM )
	endif()
endif()

# ---------------------------- benching stuff ----------------------------
set ( WITH_BENCHES OFF CACHE BOOL "Build development benches" )
if (WITH_BENCHES)
	add_subdirectory ( gbenches )
	add_feature_info ( benches WITH_BENCHES "development benchmarks" )
endif ()

if ( NOT BUILD_TESTING )
	return()
endif()

# ---------------------------- Below is pure testing stuff ----------------------------

if ( CMAKE_VERSION VERSION_GREATER 3.1.0 AND NOT DISABLE_GTESTS AND NOT TEST_SPECIAL_EXTERNAL)
	add_subdirectory ( gtests )
endif()

add_executable(tests tests.cpp)
target_link_libraries(tests PUBLIC lmanticore)

if (WITH_MYSQL)
	# compile rtestconfig.h
	hsnippet(rtestconfig testrt.conf)

	set (DATAFLD "${MANTICORE_BINARY_DIR}/test/data")
	file ( MAKE_DIRECTORY ${DATAFLD} )
	source_group ( "Testing config" FILES testrt.conf.in )
	add_executable ( testrt testrt.cpp ${MANTICORE_BINARY_DIR}/config/rtestconfig.h )
	target_compile_definitions(testrt PRIVATE "DATAFLD=\"${DATAFLD}/\"")
	target_link_libraries ( testrt PRIVATE source_mysql PUBLIC lmanticore $<$<PLATFORM_ID:Windows>:psapi.lib> )
	if (HAVE_RTESTCONFIG_H)
		target_compile_definitions(testrt PRIVATE "-DHAVE_RTESTCONFIG_H")
	endif ()

	# testrt is alive, however it needs database 'lj' accessible
	# in order to work. So, avoid it's calling till the base is persist
	# in out CI env
	if (0)

	add_test ( NAME "\"Remove dump index\""
			COMMAND ${CMAKE_COMMAND} -E remove "${DATAFLD}/dump.*" )
	SET_TESTS_PROPERTIES ( "\"Remove dump index\"" PROPERTIES FIXTURES_SETUP TESTRT )

	add_test ( NAME "\"Internal rt src/testrt\""
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND testrt )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt\"" PROPERTIES FIXTURES_REQUIRED TESTRT )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt\"" PROPERTIES RESOURCE_LOCK DbAccess )

	add_test ( NAME "\"Internal rt src/testrt step 1000\""
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND testrt 1000 )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1000\"" PROPERTIES FIXTURES_REQUIRED TESTRT )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1000\"" PROPERTIES RESOURCE_LOCK DbAccess )

	add_test ( NAME "\"Internal rt src/testrt step 100\""
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND testrt 100 )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 100\"" PROPERTIES FIXTURES_REQUIRED TESTRT )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1000\"" PROPERTIES RESOURCE_LOCK DbAccess )

	add_test ( NAME "\"Internal rt src/testrt step 10\""
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND testrt 10 )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 10\"" PROPERTIES FIXTURES_REQUIRED TESTRT )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1000\"" PROPERTIES RESOURCE_LOCK DbAccess )

	add_test ( NAME "\"Internal rt src/testrt step 1\""
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND testrt 1 )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1\"" PROPERTIES FIXTURES_REQUIRED TESTRT )
	SET_TESTS_PROPERTIES ( "\"Internal rt src/testrt step 1000\"" PROPERTIES RESOURCE_LOCK DbAccess )

	endif(0)
endif (WITH_MYSQL)

if (NOT TEST_SPECIAL_EXTERNAL)

	# since cmake 3.18 names need fixup
	include(fixup_test_name)

	# first check syntax
	if (NOT WIN32)
		find_package ( PythonInterp QUIET )
		if ( PYTHONINTERP_FOUND AND NOT DISABLE_GTESTS )
			fixup_test_name ( RESERVED_TEST "SphinxQL reserved keywords consistency" )
			message ( STATUS "python binary is ${PYTHON_EXECUTABLE}" ) # !COMMIT
			add_test ( NAME ${RESERVED_TEST}
					WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
					COMMAND ${PYTHON_EXECUTABLE} reserved.py )
			SET_TESTS_PROPERTIES ( ${RESERVED_TEST} PROPERTIES LABELS LINTER )
		endif()
	endif()

	fixup_test_name (tst "Internal src/tests")
	add_test ( NAME ${tst}
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			COMMAND "tests" )
endif()

