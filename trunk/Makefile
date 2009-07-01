#########################################################################
#
# This is the master makefile.
# Here we put all the top-level make targets, platform-independent
# rules, etc.
#
# Run 'make help' to list helpful targets.
#
#########################################################################


ifdef NOCMAKE
#########################################################################
# When not using CMake, the top-level Makefile is just a stub that
# merely includes src/make/master.mk
#########################################################################
include src/make/master.mk

else

.PHONY: all debug profile clean realclean nuke doxygen

working_dir	:= ${shell pwd}

# Figure out which architecture we're on
include ${working_dir}/src/make/detectplatform.mk

# Presence of make variables DEBUG and PROFILE cause us to make special
# builds, which we put in their own areas.
ifdef DEBUG
    variant +=.debug
endif
ifdef PROFILE
    variant +=.profile
endif

# Set up variables holding the names of platform-dependent directories
top_build_dir := build
build_dir     := ${top_build_dir}/${platform}${variant}
top_dist_dir  := dist
dist_dir      := ${top_dist_dir}/${platform}${variant}

$(info dist_dir = ${dist_dir})

MY_MAKE_FLAGS ?=
MY_CMAKE_FLAGS ?=

ifneq (${VERBOSE},)
MY_MAKE_FLAGS += VERBOSE=${VERBOSE}
endif

ifdef DEBUG
MY_CMAKE_FLAGS += -DCMAKE_BUILD_TYPE:STRING=Debug
endif

ifneq (${MYCC},)
MY_CMAKE_FLAGS += -DCMAKE_C_COMPILER:STRING=${MYCC}
endif
ifneq (${MYCXX},)
MY_CMAKE_FLAGS += -DCMAKE_CXX_COMPILER:STRING=${MYCXX}
endif

#$(info MY_CMAKE_FLAGS = ${MY_CMAKE_FLAGS})
#$(info MY_MAKE_FLAGS = ${MY_MAKE_FLAGS})

#########################################################################




#########################################################################
# Top-level documented targets

all: dist

# 'make debug' is implemented via recursive make setting DEBUG
debug:
	${MAKE} DEBUG=1 --no-print-directory

# 'make profile' is implemented via recursive make setting PROFILE
profile:
	${MAKE} PROFILE=1 --no-print-directory

# 'make cmakesetup' constructs the build directory and runs 'cmake'
# there, generating makefiles to build the project.
cmakesetup:
	cmake -E make_directory ${build_dir}
	( cd ${build_dir} ; \
	  cmake -DCMAKE_INSTALL_PREFIX=${working_dir}/${dist_dir} \
	        ${MY_CMAKE_FLAGS} \
		-DBOOST_ROOT=${BOOST_HOME} \
		../../src )

# 'make cmake' does a basic build (after first setting it up)
cmake: cmakesetup
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} )

# 'make cmakeinstall' builds everthing and installs it in 'dist'
cmakeinstall: cmake
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} install )

# 'make dist' is just a synonym for 'make cmakeinstall'
dist : cmakeinstall

# 'make fast' builds assuming cmake has already run.  Use with caution!
fast: 
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} install )

# 'make test' does a full build and then runs all tests
test: cmake
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} test )

# 'make package' builds everything and then makes an installable package 
# (platform dependent -- may be .tar.gz, .sh, .dmg, .rpm, .deb. .exe)
package: cmakeinstall
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} package )

# 'make package_source' makes an installable source package 
# (platform dependent -- may be .tar.gz, .sh, .dmg, .rpm, .deb. .exe)
package_source: cmakeinstall
	( cd ${build_dir} ; make ${MY_MAKE_FLAGS} package_source )

#clean: testclean
# 'make clean' clears out the build directory for this platform
clean:
	cmake -E remove_directory ${build_dir}

# 'make realclean' clears out both build and dist directories for this platform
realclean: clean
	cmake -E remove_directory ${dist_dir}

# 'make nuke' blows away the build and dist areas for all platforms
nuke:
	cmake -E remove_directory ${top_build_dir}
	cmake -E remove_directory ${top_dist_dir}

doxygen:
	doxygen src/doc/Doxyfile

#testclean : ${all_tests}
#	@ for f in ${all_tests} ; do \
#	    ( cd $$f ; \
#	      echo "Cleaning test $$f " ; \
#	      ./run.py -c ; \
#	    ) \
#	done

#########################################################################



# 'make help' prints important make targets
help:
	@echo "Targets:"
	@echo "  make              Build optimized binaries and libraries in ${dist_dir},"
	@echo "                        temporary build files in ${build_dir}"
	@echo "  make debug        Build unoptimized with symbols in ${dist_dir}.debug,"
	@echo "                        temporary build files in ${build_dir}.debug"
	@echo "  make profile      Build for profiling in ${dist_dir}.profile,"
	@echo "                        temporary build files in ${build_dir}.profile"
	@echo "  make clean        Remove the temporary files in ${build_dir}"
	@echo "  make realclean    Remove both ${build_dir} AND ${dist_dir}"
	@echo "  make nuke         Remove ALL of build and dist (not just ${platform})"
	@echo "  make test         Run all tests"
	@echo "  make doxygen      Build the Doxygen docs in ${top_build_dir}/doxygen"
	@echo ""
	@echo "Helpful modifiers:"
	@echo "  make VERBOSE=1 ...          Show all compilation commands"
	@echo "  make MYCC=xx MYCXX=yy ...   Use custom compilers"
	@echo ""



endif
