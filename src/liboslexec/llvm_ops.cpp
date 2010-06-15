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

#include <string>
#include <cstdarg>
#include <cstdio>

#include "oslconfig.h"
#include "oslclosure.h"
#include "oslexec_pvt.h"
using namespace OSL;
using namespace OSL::pvt;

#include <OpenEXR/ImathFun.h>

// Handy re-cast the incoming const char* as a ustring& (which we know it
// is).
#define USTR(cstr) (*((ustring *)&cstr))
#define MAT(m) (*(Matrix44 *)m)
#define VEC(v) (*(Vec3 *)v)



extern "C" void
osl_printf (const char* format_str, ...)
{
    // FIXME -- no, no, we need to take a ShadingSys ref and go through
    // the preferred output mechanisms.
    va_list args;
    va_start (args, format_str);
#if 0
    // Make super sure we know we are excuting LLVM-generated code!
    std::string newfmt = std::string("llvm: ") + format_str;
    format_str = newfmt.c_str();
#endif
    vprintf (format_str, args);
    va_end (args);
}



extern "C" void
osl_closure_clear (ClosureColor *r)
{
    r->clear ();
}

extern "C" void
osl_closure_assign (ClosureColor *r, ClosureColor *x)
{
    *r = *x;
}

extern "C" void
osl_add_closure_closure (ClosureColor *r, ClosureColor *a, ClosureColor *b)
{
    r->add (*a, *b);
}

extern "C" void
osl_mul_closure_float (ClosureColor *r, ClosureColor *a, float b)
{
    *r = *a;
    *r *= b;
}

extern "C" void
osl_mul_closure_color (ClosureColor *r, ClosureColor *a, Color3 *b)
{
    *r = *a;
    *r *= *b;
}


extern "C" void
osl_mul_mm (Matrix44 *r, Matrix44 *a, Matrix44 *b)
{
    *r = (*a) * (*b);
}

extern "C" void
osl_mul_mf (Matrix44 *r, Matrix44 *a, float b)
{
    *r = (*a) * b;
}

extern "C" void
osl_mul_m_ff (Matrix44 *r, float a, float b)
{
    float f = a * b;
    *r = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
}

extern "C" void
osl_div_mm (Matrix44 *r, Matrix44 *a, Matrix44 *b)
{
    *r = (*a) * b->inverse();
}

extern "C" void
osl_div_mf (Matrix44 *r, Matrix44 *a, float b)
{
    *r = (*a) * (1.0f/b);
}

extern "C" void
osl_div_fm (Matrix44 *r, float a, Matrix44 *b)
{
    *r = a * b->inverse();
}

extern "C" void
osl_div_m_ff (Matrix44 *r, float a, float b)
{
    float f = (b == 0) ? 0.0f : (a / b);
    *r = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
}

bool
osl_get_matrix (SingleShaderGlobal *sg, Matrix44 *r, const char *from)
{
    ShadingContext *ctx = (ShadingContext *)sg->context;
    if (USTR(from) == Strings::common ||
            USTR(from) == ctx->shadingsys().commonspace_synonym()) {
        r->makeIdentity ();
        return true;
    }
    if (USTR(from) == Strings::shader) {
        ctx->renderer()->get_matrix (*r, sg->shader2common, sg->time);
        return true;
    }
    if (USTR(from) == Strings::object) {
        ctx->renderer()->get_matrix (*r, sg->object2common, sg->time);
        return true;
    }
    return ctx->renderer()->get_matrix (*r, USTR(from), sg->time);
    // FIXME -- error report if it fails?
}

bool
osl_get_inverse_matrix (SingleShaderGlobal *sg, Matrix44 *r, const char *to)
{
    ShadingContext *ctx = (ShadingContext *)sg->context;
    if (USTR(to) == Strings::common ||
            USTR(to) == ctx->shadingsys().commonspace_synonym()) {
        r->makeIdentity ();
        return true;;
    }
    if (USTR(to) == Strings::shader) {
        ctx->renderer()->get_inverse_matrix (*r, sg->shader2common, sg->time);
        return true;
    }
    if (USTR(to) == Strings::object) {
        ctx->renderer()->get_inverse_matrix (*r, sg->object2common, sg->time);
        return true;
    }
    bool ok = ctx->renderer()->get_inverse_matrix (*r, USTR(to), sg->time);
    if (! ok) {
        r->makeIdentity ();
        //FIXME  error ("Could not get matrix '%s'", to.c_str());
    }
    return true;
}



extern "C" void
osl_prepend_matrix_from (void *sg, void *r, const char *from)
{
    Matrix44 m;
    osl_get_matrix ((SingleShaderGlobal *)sg, &m, from);
    MAT(r) = m * MAT(r);
}

extern "C" void
osl_get_from_to_matrix (void *sg, void *r, const char *from, const char *to)
{
    Matrix44 Mfrom, Mto;
    bool ok = osl_get_matrix ((SingleShaderGlobal *)sg, &Mfrom, from);
    ok &= osl_get_inverse_matrix ((SingleShaderGlobal *)sg, &Mto, to);
    MAT(r) = Mfrom * Mto;
}



// String ops

// Only define 2-arg version of concat, sort it out upstream
extern "C" const char *
osl_concat (const char *s, const char *t)
{
    return ustring::format("%s%s", s, t).c_str();
}

extern "C" int
osl_strlen (const char *s)
{
    return (int) USTR(s).length();
}

extern "C" int
osl_startswith (const char *s, const char *substr)
{
    return strncmp (s, substr, USTR(substr).length()) == 0;
}

extern "C" int
osl_endswith (const char *s, const char *substr)
{
    size_t len = USTR(substr).length();
    if (len > USTR(s).length())
        return 0;
    else
        return strncmp (s+USTR(s).length()-len, substr, len);
}

extern "C" const char *
osl_substr (const char *s, int start, int length)
{
    int slen = (int) USTR(s).length();
    int b = start;
    if (b < 0)
        b += slen;
    b = Imath::clamp (b, 0, slen);
    return ustring(s, b, Imath::clamp (length, 0, slen)).c_str();
}
