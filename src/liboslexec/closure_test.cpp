/*
Copyright (c) 2009 Sony Pictures Imageworks, et al.
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
#include <iostream>

#include <OpenImageIO/dassert.h>

#include "oslconfig.h"
#include "oslclosure.h"
using namespace OSL;
//using namespace pvt;


#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>


class MyClosure : public BSDFClosure {
public:
    MyClosure () : BSDFClosure ("my", "f") { }
    bool eval (const void *paramsptr, const Vec3 &L, const Color3 &El,
               const Vec3 &R, Color3 &Er) const
    {
        return false;
    }
    void sample (const void *paramsptr, const Vec3 &I, float randu, float randv,
                 Vec3 &R, float &pdf) const
    { }
    float pdf (const void *paramsptr, const Vec3 &I, const Vec3 &R) const
    {
        return 0;
    }
};

MyClosure myclosure;



BOOST_AUTO_TEST_CASE (closure_test_add)
{
   // avoid warnings from boost headers
    BOOST_CHECK_CLOSE(0.0f, 0.0f, 0.001f);
    BOOST_CHECK_SMALL(0.0f, 0.001f);

   // Create a closure with one component
    ClosureColor c;
    c.add_component (ClosurePrimitive::primitive (ustring("my")), Color3(.1, .1, .1));
    float f = 0.33f;
    c.set_parameter (0, 0, &f);
    BOOST_CHECK_EQUAL (c.ncomponents(), 1);

    // Add another component with different params.  It should now look
    // like two components, not combine with the others.
    c.add_component (ClosurePrimitive::primitive (ustring("my")), Color3(.4, .4, .4));
    f = 0.5;
    c.set_parameter (1, 0, &f);
    BOOST_CHECK_EQUAL (c.ncomponents(), 2);
    BOOST_CHECK_EQUAL (c.weight(1), Color3 (0.4, 0.4, 0.4));
    std::cout << "c = " << c << "\n";
}

