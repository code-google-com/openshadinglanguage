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

using namespace OSL;


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
