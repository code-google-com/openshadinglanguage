/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>

#include "oslexec.h"
#include "../liboslexec/oslexec_pvt.h"
#include "../liboslexec/llvm_headers.h"

using namespace OSL;
using namespace OSL::pvt;

int
main (int argc, const char *argv[])
{
    // Create a new shading system.
    ShadingSystem* shadingsys = ShadingSystem::create (NULL, NULL, NULL);
    printf("Created a shadingsys (%p)\n", shadingsys);
    ShadingSystemImpl* opaque = (ShadingSystemImpl*)shadingsys;
    opaque->SetupLLVM();
    shadingsys->attribute("lockgeom", 1);
    shadingsys->ShaderGroupBegin ();
    shadingsys->Shader("surface", "test", "test_layer");
    shadingsys->ShaderGroupEnd ();
    ShadingContext* ctx = ((ShadingSystemImpl*)shadingsys)->get_context();
    // Set up shader globals and a little test grid of points to shade.
    ShaderGlobals shaderglobals;
    ShadingAttribStateRef shaderstate = shadingsys->state ();
    ctx->bind(1, *shaderstate, shaderglobals);
    ShadingSystem::destroy (shadingsys);
    return EXIT_SUCCESS;
}
