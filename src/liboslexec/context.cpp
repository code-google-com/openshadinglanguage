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

#include <vector>
#include <string>
#include <cstdio>

#include <boost/foreach.hpp>
#include <boost/regex.hpp>

#include <OpenImageIO/dassert.h>
#include <OpenImageIO/sysutil.h>

#include "oslexec_pvt.h"
#include "oslops.h"

#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {

namespace pvt {   // OSL::pvt


ShadingContext::ShadingContext (ShadingSystemImpl &shadingsys) 
    : m_shadingsys(shadingsys), m_renderer(m_shadingsys.renderer()),
      m_attribs(NULL), m_dictionary(NULL)
{
    m_shadingsys.m_stat_contexts += 1;
}



ShadingContext::~ShadingContext ()
{
    m_shadingsys.m_stat_contexts -= 1;
    for (RegexMap::iterator it = m_regex_map.begin(); it != m_regex_map.end(); ++it) {
      delete it->second;
    }
    free_dict_resources ();
}



bool
ShadingContext::prepare_execution (ShaderUse use, ShadingAttribState &sas)
{
    DASSERT (use == ShadUseSurface);  // FIXME

    m_curuse = use;
    m_attribs = &sas;
    m_closures_allotted = 0;

    // Optimize if we haven't already
    ShaderGroup &sgroup (sas.shadergroup (use));
    if (sgroup.nlayers()) {
        sgroup.start_running ();
        if (! sgroup.optimized()) {
            shadingsys().optimize_group (sas, sgroup);
        }
    } else {
       // empty shader - nothing to do!
       return false; 
    }

    // Allocate enough space on the heap
    size_t heap_size_needed = sgroup.llvm_groupdata_size();
    if (heap_size_needed > m_heap.size()) {
        if (shadingsys().debug())
            shadingsys().info ("  ShadingContext %p growing heap to %llu",
                               this, (unsigned long long) heap_size_needed);
        m_heap.resize (heap_size_needed);
    }
    // Zero out the heap memory we will be using
    if (shadingsys().m_clearmemory)
        memset (&m_heap[0], 0, heap_size_needed);

    // Set up closure storage
    m_closure_pool.clear();

    // Clear the message blackboard
    m_messages.clear ();

    return true;
}



void
ShadingContext::execute (ShaderUse use, ShadingAttribState &sas,
                         ShaderGlobals &ssg)
{
    if (! prepare_execution (use, sas))
        return;

    ShaderGroup &sgroup (m_attribs->shadergroup (use));
    DASSERT (sgroup.llvm_compiled_version());
    DASSERT (sgroup.llvm_groupdata_size() <= m_heap.size());
    ssg.context = this;
    ssg.Ci = NULL;
    RunLLVMGroupFunc run_func = sgroup.llvm_compiled_version();
    run_func (&ssg, &m_heap[0]);
}



Symbol *
ShadingContext::symbol (ShaderUse use, ustring name)
{
    ShaderGroup &sgroup (attribs()->shadergroup (use));
    int nlayers = sgroup.nlayers ();
    if (sgroup.llvm_compiled_version()) {
        for (int layer = nlayers-1;  layer >= 0;  --layer) {
            int symidx = sgroup[layer]->findsymbol (name);
            if (symidx >= 0)
                return sgroup[layer]->symbol (symidx);
        }
    }
    return NULL;
}



void *
ShadingContext::symbol_data (Symbol &sym, int gridpoint)
{
    ShaderGroup &sgroup (attribs()->shadergroup ((ShaderUse)m_curuse));
    if (sgroup.llvm_compiled_version()) {
        size_t offset = sgroup.llvm_groupdata_size() * gridpoint;
        offset += sym.dataoffset();
        return &m_heap[offset];
    }
    return NULL;
}



const boost::regex &
ShadingContext::find_regex (ustring r)
{
    RegexMap::const_iterator found = m_regex_map.find (r);
    if (found != m_regex_map.end())
        return *found->second;
    // otherwise, it wasn't found, add it
    m_regex_map[r] = new boost::regex(r.c_str());
    m_shadingsys.m_stat_regexes += 1;
    // std::cerr << "Made new regex for " << r << "\n";
    return *m_regex_map[r];
}



}; // namespace pvt
}; // namespace OSL
#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif
