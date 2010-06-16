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

#include <dual.h>
#include <dual_vec.h>
#include <OpenEXR/ImathFun.h>

// Handy re-casting macros// is).
#define USTR(cstr) (*((ustring *)&cstr))
#define MAT(m) (*(Matrix44 *)m)
#define VEC(v) (*(Vec3 *)v)
#define DFLOAT(x) (*(Dual2<Float> *)x)
#define DVEC(x) (*(Dual2<Vec3> *)x)



extern "C" const char *
osl_format (const char* format_str, ...)
{
    va_list args;
    va_start (args, format_str);
    std::string s = Strutil::vformat (format_str, args);
    va_end (args);
    return ustring(s).c_str();
}


extern "C" void
osl_printf (void *sg_, const char* format_str, ...)
{
    SingleShaderGlobal *sg = (SingleShaderGlobal *)sg_;
    va_list args;
    va_start (args, format_str);
#if 0
    // Make super sure we know we are excuting LLVM-generated code!
    std::string newfmt = std::string("llvm: ") + format_str;
    format_str = newfmt.c_str();
#endif
    std::string s = Strutil::vformat (format_str, args);
    va_end (args);
    sg->context->shadingsys().message (s);
}


extern "C" void
osl_error (void *sg_, const char* format_str, ...)
{
    SingleShaderGlobal *sg = (SingleShaderGlobal *)sg_;
    va_list args;
    va_start (args, format_str);
    std::string s = Strutil::vformat (format_str, args);
    va_end (args);
    sg->context->shadingsys().error (s);
}


extern "C" void
osl_warning (void *sg_, const char* format_str, ...)
{
    SingleShaderGlobal *sg = (SingleShaderGlobal *)sg_;
    va_list args;
    va_start (args, format_str);
    std::string s = Strutil::vformat (format_str, args);
    va_end (args);
    sg->context->shadingsys().warning (s);
}



#define MAKE_UNARY_PERCOMPONENT_OP(name,floatfunc,dualfunc)         \
extern "C" float                                                    \
osl_##name##_ff (float a)                                           \
{                                                                   \
    return floatfunc(a);                                            \
}                                                                   \
                                                                    \
extern "C" void                                                     \
osl_##name##_dfdf (void *r, void *a)                                \
{                                                                   \
    DFLOAT(r) = dualfunc (DFLOAT(a));                               \
}                                                                   \
                                                                    \
extern "C" void                                                     \
osl_##name##_vv (void *r_, void *a_)                                \
{                                                                   \
    Vec3 &r (VEC(r_));                                              \
    Vec3 &a (VEC(a_));                                              \
    r[0] = floatfunc (a[0]);                                        \
    r[1] = floatfunc (a[1]);                                        \
    r[2] = floatfunc (a[2]);                                        \
}                                                                   \
                                                                    \
extern "C" void                                                     \
osl_##name##_dvdv (void *r_, void *a_)                              \
{                                                                   \
    Dual2<Vec3> &r (DVEC(r_));                                      \
    Dual2<Vec3> &a (DVEC(a_));                                      \
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */           \
    Dual2<float> ax, ay, az;                                        \
    ax = dualfunc (Dual2<float> (a.val().x, a.dx().x, a.dy().x));   \
    ay = dualfunc (Dual2<float> (a.val().y, a.dx().y, a.dy().y));   \
    az = dualfunc (Dual2<float> (a.val().z, a.dx().z, a.dy().z));   \
    /* Now swizzle back */                                          \
    r.set (Vec3( ax.val(), ay.val(), az.val()),                     \
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),                     \
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));                    \
}


#define MAKE_BINARY_PERCOMPONENT_OP(name,floatfunc,dualfunc)        \
extern "C" float osl_##name##_fff (float a, float b) {              \
    return floatfunc(a,b);                                          \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dfdfdf (void *r, void *a, void *b) {   \
    DFLOAT(r) = dualfunc (DFLOAT(a),DFLOAT(b));                     \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dffdf (void *r, float a, void *b) {    \
    DFLOAT(r) = dualfunc (Dual2<float>(a),DFLOAT(b));               \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dfdff (void *r, void *a, float b) {    \
    DFLOAT(r) = dualfunc (DFLOAT(a),Dual2<float>(b));               \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_vvv (void *r_, void *a_, void *b_) {   \
    Vec3 &r (VEC(r_));                                              \
    Vec3 &a (VEC(a_));                                              \
    Vec3 &b (VEC(b_));                                              \
    r[0] = floatfunc (a[0], b[0]);                                  \
    r[1] = floatfunc (a[1], b[1]);                                  \
    r[2] = floatfunc (a[2], b[2]);                                  \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dvdvdv (void *r_, void *a_, void *b_)  \
{                                                                   \
    Dual2<Vec3> &r (DVEC(r_));                                      \
    Dual2<Vec3> &a (DVEC(a_));                                      \
    Dual2<Vec3> &b (DVEC(b_));                                      \
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */           \
    Dual2<float> ax, ay, az;                                        \
    ax = dualfunc (Dual2<float> (a.val().x, a.dx().x, a.dy().x),    \
                   Dual2<float> (b.val().x, b.dx().x, b.dy().x));   \
    ay = dualfunc (Dual2<float> (a.val().y, a.dx().y, a.dy().y),    \
                   Dual2<float> (b.val().y, b.dx().y, b.dy().y));   \
    az = dualfunc (Dual2<float> (a.val().z, a.dx().z, a.dy().z),    \
                   Dual2<float> (b.val().z, b.dx().z, b.dy().z));   \
    /* Now swizzle back */                                          \
    r.set (Vec3( ax.val(), ay.val(), az.val()),                     \
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),                     \
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));                    \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dvvdv (void *r_, void *a_, void *b_)   \
{                                                                   \
    Dual2<Vec3> &r (DVEC(r_));                                      \
    Dual2<Vec3> a (VEC(a_), Vec3(0,0,0), Vec3(0,0,0));              \
    Dual2<Vec3> &b (DVEC(b_));                                      \
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */           \
    Dual2<float> ax, ay, az;                                        \
    ax = dualfunc (Dual2<float> (a.val().x, a.dx().x, a.dy().x),    \
                   Dual2<float> (b.val().x, b.dx().x, b.dy().x));   \
    ay = dualfunc (Dual2<float> (a.val().y, a.dx().y, a.dy().y),    \
                   Dual2<float> (b.val().y, b.dx().y, b.dy().y));   \
    az = dualfunc (Dual2<float> (a.val().z, a.dx().z, a.dy().z),    \
                   Dual2<float> (b.val().z, b.dx().z, b.dy().z));   \
    /* Now swizzle back */                                          \
    r.set (Vec3( ax.val(), ay.val(), az.val()),                     \
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),                     \
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));                    \
}                                                                   \
                                                                    \
extern "C" void osl_##name##_dvdvv (void *r_, void *a_, void *b_)   \
{                                                                   \
    Dual2<Vec3> &r (DVEC(r_));                                      \
    Dual2<Vec3> &a (DVEC(a_));                                      \
    Dual2<Vec3> b (VEC(b_), Vec3(0,0,0), Vec3(0,0,0));              \
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */           \
    Dual2<float> ax, ay, az;                                        \
    ax = dualfunc (Dual2<float> (a.val().x, a.dx().x, a.dy().x),    \
                   Dual2<float> (b.val().x, b.dx().x, b.dy().x));   \
    ay = dualfunc (Dual2<float> (a.val().y, a.dx().y, a.dy().y),    \
                   Dual2<float> (b.val().y, b.dx().y, b.dy().y));   \
    az = dualfunc (Dual2<float> (a.val().z, a.dx().z, a.dy().z),    \
                   Dual2<float> (b.val().z, b.dx().z, b.dy().z));   \
    /* Now swizzle back */                                          \
    r.set (Vec3( ax.val(), ay.val(), az.val()),                     \
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),                     \
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));                    \
}


MAKE_UNARY_PERCOMPONENT_OP (sin, sinf, sin)
MAKE_UNARY_PERCOMPONENT_OP (cos, cosf, cos)
MAKE_UNARY_PERCOMPONENT_OP (tan, tanf, tan)


inline float safe_asinf (float x) {
    if (x >=  1.0f) return  M_PI/2;
    if (x <= -1.0f) return -M_PI/2;
    return std::asin (x);
}

inline float safe_acosf (float x) {
    if (x >=  1.0f) return 0.0f;
    if (x <= -1.0f) return M_PI;
    return std::acos (x);
}

MAKE_UNARY_PERCOMPONENT_OP (asin, safe_asinf, asin)
MAKE_UNARY_PERCOMPONENT_OP (acos, safe_acosf, acos)
MAKE_UNARY_PERCOMPONENT_OP (atan, std::atan, atan)
MAKE_BINARY_PERCOMPONENT_OP (atan2, std::atan2, atan2)
MAKE_UNARY_PERCOMPONENT_OP (sinh, std::sinh, sinh)
MAKE_UNARY_PERCOMPONENT_OP (cosh, std::cosh, cosh)
MAKE_UNARY_PERCOMPONENT_OP (tanh, std::tanh, tanh)

inline float safe_log (float f) {
    if (f <= 0.0f)
        return -std::numeric_limits<float>::max();
    else
        return std::log (f);
}

inline float safe_log2(float x) {
    if (x <= 0.0f)
        return -std::numeric_limits<float>::max();
    else
        return log2f(x);
}

inline float safe_log10(float x) {
    if (x <= 0.0f)
        return -std::numeric_limits<float>::max();
    else
        return log10f(x);
}

inline float safe_logb (float f) {
    if (f == 0.0f) {
        // m_exec->error ("attempted to compute logb(%g)", f);
        return -std::numeric_limits<float>::max();
    } else {
        return logbf (f);
    }
}

inline Dual2<float> logb (const Dual2<float> &f) {
    // FIXME - punt on derivs
    return Dual2<float> (safe_logb(f.val()), 0.0, 0.0);
}


MAKE_UNARY_PERCOMPONENT_OP (log, safe_log, log)
MAKE_UNARY_PERCOMPONENT_OP (log2, safe_log2, log2)
MAKE_UNARY_PERCOMPONENT_OP (log10, safe_log10, log10)
MAKE_UNARY_PERCOMPONENT_OP (logb, safe_logb, logb)
MAKE_UNARY_PERCOMPONENT_OP (exp, std::exp, exp)
MAKE_UNARY_PERCOMPONENT_OP (exp2, exp2f, exp2)
MAKE_UNARY_PERCOMPONENT_OP (expm1, expm1f, expm1)
MAKE_BINARY_PERCOMPONENT_OP (pow, safe_pow, pow)
MAKE_UNARY_PERCOMPONENT_OP (erf, erff, erf)
MAKE_UNARY_PERCOMPONENT_OP (erfc, erfcf, erfc)

// Mixed vec pow(vec,float)
extern "C" void osl_pow_vvf (void *r_, void *a_, float b) {
    Vec3 &r (VEC(r_));
    Vec3 &a (VEC(a_));
    r[0] = safe_pow (a[0], b);
    r[1] = safe_pow (a[1], b);
    r[2] = safe_pow (a[2], b);
}

extern "C" void osl_pow_dvdvdf (void *r_, void *a_, void *b_)
{
    Dual2<Vec3> &r (DVEC(r_));
    Dual2<Vec3> &a (DVEC(a_));
    Dual2<float> &b (DFLOAT(b_));
    Dual2<float> ax, ay, az;
    ax = pow (Dual2<float> (a.val().x, a.dx().x, a.dy().x),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    ay = pow (Dual2<float> (a.val().y, a.dx().y, a.dy().y),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    az = pow (Dual2<float> (a.val().z, a.dx().z, a.dy().z),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    /* Now swizzle back */
    r.set (Vec3( ax.val(), ay.val(), az.val()),
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));
}

extern "C" void osl_pow_dvvdf (void *r_, void *a_, void *b_)
{
    Dual2<Vec3> &r (DVEC(r_));
    Vec3 &a (VEC(a_));
    Dual2<float> &b (DFLOAT(b_));
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */
    Dual2<float> ax, ay, az;
    ax = pow (Dual2<float> (a.x),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    ay = pow (Dual2<float> (a.y),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    az = pow (Dual2<float> (a.z),
                   Dual2<float> (b.val(), b.dx(), b.dy()));
    /* Now swizzle back */
    r.set (Vec3( ax.val(), ay.val(), az.val()),
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));
}

extern "C" void osl_pow_dvdvf (void *r_, void *a_, float b_)
{
    Dual2<Vec3> &r (DVEC(r_));
    Dual2<Vec3> &a (DVEC(a_));
    Dual2<float> b (b_);
    /* Swizzle the Dual2<Vec3>'s into 3 Dual2<float>'s */
    Dual2<float> ax, ay, az;
    ax = pow (Dual2<float> (a.val().x, a.dx().x, a.dy().x), b);
    ay = pow (Dual2<float> (a.val().y, a.dx().y, a.dy().y), b);
    az = pow (Dual2<float> (a.val().z, a.dx().z, a.dy().z), b);
    /* Now swizzle back */
    r.set (Vec3( ax.val(), ay.val(), az.val()),
           Vec3( ax.dx(),  ay.dx(),  az.dx() ),
           Vec3( ax.dy(),  ay.dy(),  az.dy() ));
}




// Closure functions

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



// Matrix ops

extern "C" void
osl_mul_mm (void *r, void *a, void *b)
{
    MAT(r) = MAT(a) * MAT(b);
}

extern "C" void
osl_mul_mf (void *r, void *a, float b)
{
    MAT(r) = MAT(a) * b;
}

extern "C" void
osl_mul_m_ff (void *r, float a, float b)
{
    float f = a * b;
    MAT(r) = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
}

extern "C" void
osl_div_mm (void *r, void *a, void *b)
{
    MAT(r) = MAT(a) * MAT(b).inverse();
}

extern "C" void
osl_div_mf (void *r, void *a, float b)
{
    MAT(r) = MAT(a) * (1.0f/b);
}

extern "C" void
osl_div_fm (void *r, float a, void *b)
{
    MAT(r) = a * MAT(b).inverse();
}

extern "C" void
osl_div_m_ff (void *r, float a, float b)
{
    float f = (b == 0) ? 0.0f : (a / b);
    MAT(r) = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
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

extern "C" void
osl_transpose_mm (void *r, void *m)
{
    MAT(r) = MAT(m).transposed();
}

// Calculate the determinant of a 2x2 matrix.
template <typename F>
inline F det2x2(F a, F b, F c, F d)
{
    return a * d - b * c;
}

// calculate the determinant of a 3x3 matrix in the form:
//     | a1,  b1,  c1 |
//     | a2,  b2,  c2 |
//     | a3,  b3,  c3 |
template <typename F>
inline F det3x3(F a1, F a2, F a3, F b1, F b2, F b3, F c1, F c2, F c3)
{
    return a1 * det2x2( b2, b3, c2, c3 )
         - b1 * det2x2( a2, a3, c2, c3 )
         + c1 * det2x2( a2, a3, b2, b3 );
}

// calculate the determinant of a 4x4 matrix.
template <typename F>
inline F det4x4(const Imath::Matrix44<F> &m)
{
    // assign to individual variable names to aid selecting correct elements
    F a1 = m[0][0], b1 = m[0][1], c1 = m[0][2], d1 = m[0][3];
    F a2 = m[1][0], b2 = m[1][1], c2 = m[1][2], d2 = m[1][3];
    F a3 = m[2][0], b3 = m[2][1], c3 = m[2][2], d3 = m[2][3];
    F a4 = m[3][0], b4 = m[3][1], c4 = m[3][2], d4 = m[3][3];
    return a1 * det3x3( b2, b3, b4, c2, c3, c4, d2, d3, d4)
         - b1 * det3x3( a2, a3, a4, c2, c3, c4, d2, d3, d4)
         + c1 * det3x3( a2, a3, a4, b2, b3, b4, d2, d3, d4)
         - d1 * det3x3( a2, a3, a4, b2, b3, b4, c2, c3, c4);
}

extern "C" float
osl_determinant_fm (void *m)
{
    return det4x4 (MAT(m));
}



// Vector ops

extern "C" void
osl_prepend_point_from (void *sg, void *v, const char *from)
{
    Matrix44 M;
    osl_get_matrix ((SingleShaderGlobal *)sg, &M, from);
    M.multVecMatrix (VEC(v), VEC(v));
}

extern "C" void
osl_prepend_vector_from (void *sg, void *v, const char *from)
{
    Matrix44 M;
    osl_get_matrix ((SingleShaderGlobal *)sg, &M, from);
    M.multDirMatrix (VEC(v), VEC(v));
}

extern "C" void
osl_prepend_normal_from (void *sg, void *v, const char *from)
{
    Matrix44 M;
    osl_get_matrix ((SingleShaderGlobal *)sg, &M, from);
    M = M.inverse().transpose();
    M.multDirMatrix (VEC(v), VEC(v));
}


extern "C" float
osl_dot_fvv (void *a, void *b)
{
    return VEC(a).dot (VEC(b));
}

extern "C" void
osl_dot_dfdvdv (void *result, void *a, void *b)
{
    DFLOAT(result) = dot (DVEC(a), DVEC(b));
}

extern "C" void
osl_dot_dfdvv (void *result, void *a, void *b_)
{
    Dual2<Vec3> b (VEC(b_));
    osl_dot_dfdvdv (result, a, &b);
}

extern "C" void
osl_dot_dfvdv (void *result, void *a_, void *b)
{
    Dual2<Vec3> a (VEC(a_));
    osl_dot_dfdvdv (result, &a, b);
}


extern "C" void
osl_cross_vvv (void *result, void *a, void *b)
{
    VEC(result) = VEC(a).cross (VEC(b));
}

extern "C" void
osl_cross_dvdvdv (void *result, void *a, void *b)
{
    DVEC(result) = cross (DVEC(a), DVEC(b));
}

extern "C" void
osl_cross_dvdvv (void *result, void *a, void *b_)
{
    Dual2<Vec3> b (VEC(b_));
    osl_cross_dvdvdv (result, a, &b);
}

extern "C" void
osl_cross_dvvdv (void *result, void *a_, void *b)
{
    Dual2<Vec3> a (VEC(a_));
    osl_cross_dvdvdv (result, &a, b);
}


extern "C" float
osl_length_fv (void *a)
{
    return VEC(a).length();
}

extern "C" void
osl_length_dfdv (void *result, void *a)
{
    DVEC(result) = length(DVEC(a));
}


extern "C" float
osl_distance_fvv (void *a_, void *b_)
{
    const Vec3 &a (VEC(a_));
    const Vec3 &b (VEC(b_));
    float x = a[0] - b[0];
    float y = a[1] - b[1];
    float z = a[2] - b[2];
    return sqrtf (x*x + y*y + z*z);
}

extern "C" void
osl_distance_dfdvdv (void *result, void *a, void *b)
{
    DVEC(result) = distance (DVEC(a), DVEC(b));
}

extern "C" void
osl_distance_dfdvv (void *result, void *a, void *b)
{
    DVEC(result) = distance (DVEC(a), VEC(b));
}

extern "C" void
osl_distance_dfvdv (void *result, void *a, void *b)
{
    DVEC(result) = distance (VEC(a), DVEC(b));
}


extern "C" void
osl_normalize_vv (void *result, void *a)
{
    VEC(result) = VEC(a).normalized();
}

extern "C" void
osl_normalize_dvdv (void *result, void *a)
{
    DVEC(result) = normalize(DVEC(a));
}





// String ops

// Only define 2-arg version of concat, sort it out upstream
extern "C" const char *
osl_concat_sss (const char *s, const char *t)
{
    return ustring::format("%s%s", s, t).c_str();
}

extern "C" int
osl_strlen_is (const char *s)
{
    return (int) USTR(s).length();
}

extern "C" int
osl_startswith_iss (const char *s, const char *substr)
{
    return strncmp (s, substr, USTR(substr).length()) == 0;
}

extern "C" int
osl_endswith_iss (const char *s, const char *substr)
{
    size_t len = USTR(substr).length();
    if (len > USTR(s).length())
        return 0;
    else
        return strncmp (s+USTR(s).length()-len, substr, len);
}

extern "C" const char *
osl_substr_ssii (const char *s, int start, int length)
{
    int slen = (int) USTR(s).length();
    int b = start;
    if (b < 0)
        b += slen;
    b = Imath::clamp (b, 0, slen);
    return ustring(s, b, Imath::clamp (length, 0, slen)).c_str();
}
