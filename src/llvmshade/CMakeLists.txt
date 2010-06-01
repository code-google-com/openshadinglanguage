SET ( llvmshade_srcs llvmshade.cpp )
ADD_EXECUTABLE ( llvmshade ${llvmshade_srcs} )
TARGET_LINK_LIBRARIES ( llvmshade oslexec oslquery )
INSTALL ( TARGETS llvmshade RUNTIME DESTINATION bin )
