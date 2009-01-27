# Template for the makefile for an individual src/* directory.
# Fill in the blanks below.

# License and copyright goes here

# Name of the binary or library whose source is in this directory.
# Do NOT include .exe or any other suffix.
local_name := oslc

# Name of all source files in this directory
local_src := oslcmain.cpp

# Extra objects from other libs we need to compile this library or binary
local_extra_objs := 

# Extra static libs needed to compile this module (leave blank if this
# module is not for a binary executable)
local_libs := 

# Extra shared libs needed to compile this module (leave blank if this
# module is not for a binary executable)
local_shlibs := liboslcomp

# ld flags needed for this module
local_ldflags := ${OPENIMAGEIO_LINK}



## Include ONE of the includes below, depending on whether this module
## constructs a binary executable, a static library, or a shared library
## (DLL).

include ${src_make_dir}/bin.mk
