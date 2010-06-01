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
